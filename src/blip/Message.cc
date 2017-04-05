//
//  Message.cc
//  LiteCore
//
//  Created by Jens Alfke on 1/2/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Message.hh"
#include "BLIPConnection.hh"
#include "BLIPInternal.hh"
#include "FleeceCpp.hh"
#include "varint.hh"
#include <zlc/zlibcomplete.hpp>
#include <algorithm>
#include <assert.h>

using namespace std;
using namespace fleece;

namespace litecore { namespace blip {


    // How many bytes to receive before sending an ACK
    static const size_t kIncomingAckThreshold = 50000;


    void Message::sendProgress(MessageProgress::State state,
                               MessageSize bytesSent, MessageSize bytesReceived,
                               MessageIn *reply) {
        if (_onProgress)
            _onProgress({state, bytesSent, bytesReceived, reply});
    }


#pragma mark - MESSAGE OUT:


    MessageOut::MessageOut(Connection *connection,
                           FrameFlags flags,
                           alloc_slice payload,
                           MessageNo number)
    :Message(flags, number)
    ,_connection(connection)
    ,_payload(payload)
    {
        assert(payload.size <= UINT32_MAX);
    }


    slice MessageOut::nextFrameToSend(size_t maxSize, FrameFlags &outFlags) {
        size_t size = min(maxSize, _payload.size - _bytesSent);
        slice frame = _payload(_bytesSent, size);
        _bytesSent += size;
        _unackedBytes += size;
        outFlags = flags();
        MessageProgress::State state;
        if (_bytesSent < _payload.size) {
            outFlags = (FrameFlags)(outFlags | kMoreComing);
            state = MessageProgress::kSending;
        } else if (noReply()) {
            state = MessageProgress::kComplete;
        } else {
            state = MessageProgress::kAwaitingReply;
        }
        sendProgress(state, _bytesSent, 0, nullptr);
        return frame;
    }


    void MessageOut::receivedAck(uint32_t byteCount) {
        if (byteCount <= _bytesSent)
            _unackedBytes = min(_unackedBytes, (uint32_t)(_bytesSent - byteCount));
    }


    MessageIn* MessageOut::createResponse() {
        if (type() != kRequestType || noReply())
            return nullptr;
        // Note: The MessageIn's flags will be updated when the 1st frame of the response arrives;
        // the type might become kErrorType, and kUrgent or kCompressed might be set.
        return new MessageIn(_connection, (FrameFlags)kResponseType, _number,
                             _onProgress, _payload.size);
    }


#pragma mark - MESSAGE IN:


    MessageIn::~MessageIn()
    {
        
    }

    MessageIn::MessageIn(Connection *connection, FrameFlags flags, MessageNo n,
                         MessageProgressCallback onProgress, MessageSize outgoingSize)
    :Message(flags, n)
    ,_connection(connection)
    ,_outgoingSize(outgoingSize)
    {
        _onProgress = onProgress;
    }


    MessageIn::ReceiveState MessageIn::receivedFrame(slice frame, FrameFlags frameFlags) {
        ReceiveState state = kOther;
        MessageSize bytesReceived = frame.size;
        {
            // First, lock the mutex:
            lock_guard<mutex> lock(_receiveMutex);
            if (_in) {
                bytesReceived += _in->bytesWritten();
            } else {
                // On first frame, update my flags and allocate the Writer:
                assert(_number > 0);
                _flags = (FrameFlags)(frameFlags & ~kMoreComing);
                _connection->log("Receiving %s #%llu, flags=%02x",
                                 kMessageTypeNames[type()], _number, flags());
                _in.reset(new fleeceapi::JSONEncoder);
                // Get the length of the properties, and move `frame` past the length field:
                if (!ReadUVarInt32(&frame, &_propertiesSize))
                    throw "frame too small";
            }

            if (!_properties && (_in->bytesWritten() + frame.size) >= _propertiesSize) {
                // OK, we now have the complete properties:
                size_t remaining = _propertiesSize - _in->bytesWritten();
                _in->writeRaw({frame.buf, remaining});
                frame.moveStart(remaining);
                _properties = _in->finish();
                if (_properties.size > 0 && _properties[_properties.size - 1] != 0)
                    throw "message properties not null-terminated";
                _in->reset();
                state = kBeginning;
            }

            _unackedBytes += frame.size;
            if (_unackedBytes >= kIncomingAckThreshold) {
                // Send an ACK every 50k bytes:
                MessageType msgType = isResponse() ? kAckResponseType : kAckRequestType;
                uint8_t buf[kMaxVarintLen64];
                alloc_slice payload(buf, PutUVarInt(buf, bytesReceived));
                Retained<MessageOut> ack = new MessageOut(_connection,
                                                          (FrameFlags)(msgType | kUrgent | kNoReply),
                                                          payload,
                                                          _number);
                _connection->send(ack);
                _unackedBytes = 0;
            }

            if (_properties && (_flags & kCompressed)) {
                if (!_decompressor)
                    _decompressor.reset( new zlibcomplete::GZipDecompressor );
                string output = _decompressor->decompress(frame.asString());
                if (output.empty())
                    throw "invalid gzipped data";
                _in->writeRaw(slice(output));
            } else {
                _in->writeRaw(frame);
            }

            if (!(frameFlags & kMoreComing)) {
                // Completed!
                if (!_properties)
                    throw "message ends before end of properties";
                _body = _in->finish();
                _in.reset();
                _decompressor.reset();
                _complete = true;

                _connection->log("Finished receiving %s #%llu, flags=%02x",
                                 kMessageTypeNames[type()], _number, flags());
                state = kEnd;
            }
        }
        // ...mutex is now unlocked

        // Send progress. ("kReceivingReply" is somewhat misleading if this isn't a reply.)
        sendProgress(state == kEnd ? MessageProgress::kComplete : MessageProgress::kReceivingReply,
                     _outgoingSize, bytesReceived,
                     (_properties ? this : nullptr));
        return state;
    }


    void MessageIn::setProgressCallback(MessageProgressCallback callback) {
        lock_guard<mutex> lock(_receiveMutex);
        _onProgress = callback;
    }


    bool MessageIn::isComplete() const {
        lock_guard<mutex> lock(const_cast<MessageIn*>(this)->_receiveMutex);
        return _complete;
    }


#pragma mark - MESSAGE BODY:


    alloc_slice MessageIn::body() const {
        lock_guard<mutex> lock(const_cast<MessageIn*>(this)->_receiveMutex);
        return _body;
    }


    fleeceapi::Value MessageIn::JSONBody() {
        lock_guard<mutex> lock(_receiveMutex);
        if (!_bodyAsFleece)
            _bodyAsFleece = FLData_ConvertJSON({_body.buf, _body.size}, nullptr);
        return fleeceapi::Value::fromData(_bodyAsFleece);
    }


    alloc_slice MessageIn::extractBody() {
        lock_guard<mutex> lock(_receiveMutex);
        alloc_slice body = _body;
        if (body) {
            _body = nullslice;
        } else if (_in) {
            body = _in->finish();
            _in->reset();
        }
        return body;
    }


#pragma mark - RESPONSES:


    void MessageIn::respond(MessageBuilder &mb) {
        if (noReply()) {
            _connection->log("Ignoring attempt to respond to a noReply message");
            return;
        }
        if (mb.type == kRequestType)
            mb.type = kResponseType;
        Retained<MessageOut> message = new MessageOut(_connection, mb, _number);
        _connection->send(message);
    }


    void MessageIn::respondWithError(Error err) {
        if (!noReply()) {
            MessageBuilder mb(this);
            mb.makeError(err);
            respond(mb);
        }
    }


    void MessageIn::notHandled() {
        respondWithError({"BLIP"_sl, 404, "no handler for message"_sl});
    }


#pragma mark - PROPERTIES:


    slice MessageIn::property(slice property) const {
        uint8_t specbuf[1] = { MessageBuilder::tokenizeProperty(property) };
        if (specbuf[0])
            property = slice(&specbuf, 1);

        // Note: using strlen here is safe. It can't fall off the end of _properties, because the
        // receivedFrame() method has already verified that _properties ends with a zero byte.
        // OPT: This lookup isn't very efficient. If it turns out to be a hot-spot, we could cache
        // the starting point of every property string.
        auto key = (const char*)_properties.buf;
        auto end = (const char*)_properties.end();
        while (key < end) {
            auto endOfKey = key + strlen(key);
            auto val = endOfKey + 1;
            if (val >= end)
                break;  // illegal: missing value
            auto endOfVal = val + strlen(val);
            if (property == slice(key, endOfKey))
                return slice(val, endOfVal);
            key = endOfVal + 1;
        }
        return nullslice;
    }


    long MessageIn::intProperty(slice name, long defaultValue) const {
        string value = property(name).asString();
        if (value.empty())
            return defaultValue;
        char *end;
        long result = strtol(value.c_str(), &end, 10);
        if (*end != '\0')
            return defaultValue;
        return result;
    }


    bool MessageIn::boolProperty(slice name, bool defaultValue) const {
        slice value = property(name);
        if (value.caseEquivalent("true"_sl) || value.caseEquivalent("YES"_sl))
            return true;
        else if (value.caseEquivalent("false"_sl) || value.caseEquivalent("NO"_sl))
            return false;
        else
            return intProperty(name, defaultValue) != 0;
    }

    
    Error MessageIn::getError() const {
        if (!isError())
            return Error();
        return Error(property("Error-Domain"_sl),
                     (int) intProperty("Error-Code"_sl),
                     body());
    }

} }
