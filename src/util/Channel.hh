//
// Channel.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>

namespace litecore { namespace actor {

    /** A simple thread-safe producer/consumer queue. */
    template <class T>
    class Channel {
    public:

        /** Pushes a new value to the front of the queue.
            @return  True if the queue was empty before the push. */
        bool push(const T &t);

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            If the queue is closed and empty, returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T pop(bool &empty)               {return pop(empty, true);}

        /** Pops the next value from the end of the queue.
            If the queue is empty, immediately returns a default (zero) T.
            @param empty  Will be set to true if the queue is now empty. */
        T popNoWaiting(bool &empty)      {return pop(empty, false);}

        /** Pops the next value from the end of the queue.
            If the queue is empty, blocks until another thread adds something to the queue.
            If the queue is closed and empty, returns a default (zero) T. */
        T pop() {
            bool more;
            return pop(more);
        }

        /** Returns the front item of the queue without popping it. The queue MUST be non-empty. */
        const T& front() const;

        /** Returns the number of items in the queue. */
        size_t size() const;

        /** When the queue is closed, after it empties all pops will return immediately with a 
            default T value instead of blocking. */
        void close();

    protected:
        std::mutex _mutex;
        
    private:
        T pop(bool &empty, bool wait);

        std::condition_variable _cond;
        std::queue<T> _queue;
        bool _closed {false};
    };
    
} }
