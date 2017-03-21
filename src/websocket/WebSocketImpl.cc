//
//  WebSocketImpl.cc
//  StreamTaskTest
//
//  Created by Jens Alfke on 3/15/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include "StringUtil.hh"
#include <string>

using namespace fleece;

// The rest of the implementation of uWS::WebSocketProtocol, which calls into WebSocket:
namespace uWS {

    static constexpr size_t kMaxMessageLength = 1<<20;

    static constexpr size_t kSendBufferSize = 64 * 1024;


    // The `user` parameter points to the owning WebSocketImpl object.
    #define _sock ((litecore::websocket::WebSocketImpl*)user)


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::setCompressed(void *user) {
        return false;   //TODO: Implement compression
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::refusePayloadLength(void *user, int length) {
        return length > kMaxMessageLength;
    }


    template <const bool isServer>
    void WebSocketProtocol<isServer>::forceClose(void *user) {
        _sock->disconnect();
    }


    template <const bool isServer>
    bool WebSocketProtocol<isServer>::handleFragment(char *data,
                                                     size_t length,
                                                     unsigned int remainingBytes,
                                                     int opCode,
                                                     bool fin,
                                                     void *user)
    {
        // WebSocketProtocol expects this method to return true on error, but this confuses me
        // so I'm having my code return false on error, hence the `!`. --jpa
        return ! _sock->handleFragment(data, length, remainingBytes, opCode, fin);
    }


    // Explicitly generate code for template methods:
    
    //template class WebSocketProtocol<SERVER>;
    template class WebSocketProtocol<CLIENT>;
    
}


#pragma mark - WEBSOCKETIMPL:


// Implementation of WebSocketImpl:
namespace litecore { namespace websocket {

    using namespace uWS;

    static LogDomain WSLogDomain("WS");


    WebSocketImpl::WebSocketImpl(ProviderImpl &provider, const Address &address)
    :WebSocket(provider, address)
    ,Logging(WSLogDomain)
    ,_protocol(new ClientProtocol)
    { }

    WebSocketImpl::~WebSocketImpl()
    { }


    std::string WebSocketImpl::loggingIdentifier() const {
        return address();
    }


    void WebSocketImpl::connect() {    // called by base class's connect(Address)
        provider().openSocket(this);
    }

    void WebSocketImpl::disconnect() {
        provider().closeSocket(this);
    }

    void WebSocketImpl::onConnect() {
        _timeConnected.start();
        delegate().onWebSocketConnect();
    }


    bool WebSocketImpl::send(fleece::slice message, bool binary) {
        return sendOp(message, binary ? uWS::BINARY : uWS::TEXT);
    }


    bool WebSocketImpl::sendOp(fleece::slice message, int opcode) {
        alloc_slice frame;
        bool writeable;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_closeSent && opcode != CLOSE)
                return false;
            frame.resize(message.size + 10); // maximum space needed
            frame.size = ClientProtocol::formatMessage((char*)frame.buf,
                                                       (const char*)message.buf, message.size,
                                                       (uWS::OpCode)opcode, message.size, false);
            _bufferedBytes += frame.size;
            writeable = (_bufferedBytes <= kSendBufferSize);
        }
        provider().sendBytes(this, frame);
        return writeable;
    }


    void WebSocketImpl::onWriteComplete(size_t size) {
        bool notify, disconnect;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _bytesSent += size;
            notify = (_bufferedBytes > kSendBufferSize);
            _bufferedBytes -= size;
            if (_bufferedBytes > kSendBufferSize)
                notify = false;

            disconnect = (_closeSent && _closeReceived && _bufferedBytes == 0);
        }

        if (disconnect) {
            // My close message has gone through; now I can disconnect:
            log("sent close echo; disconnecting socket now");
            provider().closeSocket(this);
        } else if (notify) {
            delegate().onWebSocketWriteable();
        }
    }


    void WebSocketImpl::onReceive(slice data) {
        {
            // Lock the mutex; this protects all methods (below) involved in receiving,
            // since they're called from this one.
            std::lock_guard<std::mutex> lock(_mutex);
            
            _bytesReceived += data.size;
            _protocol->consume((char*)data.buf, (unsigned)data.size, this);
            // ... this will call handleFragment(), below
        }
        provider().receiveComplete(this, data.size);
    }


    // Called from inside _protocol->consume()
    bool WebSocketImpl::handleFragment(char *data,
                                       size_t length,
                                       unsigned int remainingBytes,
                                       int opCode,
                                       bool fin)
    {
        // Beginning:
        if (!_curMessage) {
            _curOpCode = opCode;
            _curMessageCapacity = length + remainingBytes;
            _curMessage.reset(_curMessageCapacity);
            _curMessage.size = 0;
        }

        // Body:
        if (_curMessage.size + length > _curMessageCapacity)
            return false; // overflow!
        memcpy((void*)_curMessage.end(), data, length);
        _curMessage.size += length;

        // End:
        if (fin && remainingBytes == 0) {
            return receivedMessage(_curOpCode, std::move(_curMessage));
            assert(!_curMessage);
        }
        return true;
    }


    bool WebSocketImpl::receivedMessage(int opCode, alloc_slice message) {
        switch (opCode) {
            case TEXT:
                if (!ClientProtocol::isValidUtf8((unsigned char*)message.buf,
                                                            message.size))
                    return false;
                // fall through:
            case BINARY:
                delegate().onWebSocketMessage(message, (opCode==BINARY));
                return true;
            case CLOSE:
                return receivedClose(message);
            case PING:
                send(message, PONG);
                return true;
            case PONG:
                //receivedPong(message);
                return true;
            default:
                return false;
        }
    }


#pragma mark - CLOSING:


    // See <https://tools.ietf.org/html/rfc6455#section-7>


    // Initiates a request to close the connection cleanly.
    void WebSocketImpl::close(int status, fleece::slice message) {
        log("Requesting close with status=%d, message='%.*s'", status, SPLAT(message));
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_closeSent || _closeReceived)
                return;
            _closeSent = true;
            _closeMessage = alloc_slice(2 + message.size);
            auto size = ClientProtocol::formatClosePayload((char*)_closeMessage.buf,
                                                           (uint16_t)status,
                                                           (char*)message.buf, message.size);
            assert(size <= _closeMessage.size);
            _closeMessage.size = size;
        }
        sendOp(_closeMessage, uWS::CLOSE);
    }


    // Handles a close message received from the peer.
    bool WebSocketImpl::receivedClose(slice message) {
        if (_closeReceived)
            return false;
        _closeReceived = true;
        if (_closeSent) {
            // I initiated the close; the peer has confirmed, so disconnect the socket now:
            log("Close confirmed by peer; disconnecting socket now");
            provider().closeSocket(this);
        } else {
            // Peer is initiating a close. Save its message and echo it:
            if (willLog()) {
                auto close = ClientProtocol::parseClosePayload((char*)message.buf, message.size);
                log("Client is requesting close (%d '%.*s'); echoing it",
                    close.code, (int)close.length, close.message);
            }
            _closeMessage = message;
            sendOp(message, uWS::CLOSE);
        }
        return true;
    }


    // Called when the underlying socket closes.
    void WebSocketImpl::onClose(int err_no) {
        CloseStatus status = { };
        {
            std::lock_guard<std::mutex> lock(_mutex);
            bool expected = (_closeSent && _closeReceived);
            if (!expected)
                log("Unexpected socket disconnect! (errno=%d)", err_no);
            else if (err_no == 0)
                log("Socket disconnected cleanly");
            else
                log("Socket disconnect expected, but errno=%d", err_no);

            _timeConnected.stop();
            double t = _timeConnected.elapsed();
            log("sent %llu bytes, rcvd %llu, in %.3f sec (%.0f/sec, %.0f/sec)",
                _bytesSent, _bytesReceived, t,
                _bytesSent/t, _bytesReceived/t);

            if (err_no == 0) {
                status.reason = kWebSocketClose;
                if (!_closeSent || !_closeReceived)
                    status.code = kCodeAbnormal;
                else if (!_closeMessage)
                    status.code = kCodeNormal;
                else {
                    auto msg = ClientProtocol::parseClosePayload((char*)_closeMessage.buf,
                                                                 _closeMessage.size);
                    status.code = msg.code ?: kCodeStatusCodeExpected;
                    status.message = slice(msg.message, msg.length);
                }
            } else {
                status.reason = kPOSIXError;
                status.code = err_no;
            }
            _closeMessage = nullslice;
        }
        delegate().onWebSocketClose(status);
    }

} }