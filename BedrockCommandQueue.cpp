#include <libstuff/libstuff.h>
#include "BedrockCommand.h"
#include "BedrockCommandQueue.h"

void BedrockCommandQueue::push(BedrockCommand&& item)
{
    SAUTOLOCK(_queueMutex);
    auto& queue = _commandQueue[item.priority];
    queue.emplace(item.priority, move(item));
    _queueCondition.notify_one();
}

BedrockCommand BedrockCommandQueue::get(uint64_t timeoutUS) {
    unique_lock<mutex> queueLock(_queueMutex);

    // NOTE:
    // So here's the challenge. Say there's work in the queue, but it's not ready yet. Someone calls: get(1000000),
    // and nothing gets added to the queue during that second (which would wake someone up to process whatever is next,
    // which isn't necessarily the same thing that's added). BUT, some work in the queue comes due during that wait. Do
    // we want to try to implement something that would wake up as soon as that came due? It may not be worth the
    // effort.
    //
    // How might we do this?
    // If we keep the timestamp of the next command scheduled in the future, and always wake up when we hit that, then
    // we'll never miss a command. We could keep a set of timestamps to facilitate this, but we'd need to remove them
    // if they'd passed.
    //
    // Update: we don't have to wait until the next *future* timestamp, we can just wait until the *next* timestamp,
    // because if it's not in the future, we can operate on it immediately.

    // If there's already work in the queue, just return some.
    try {
        return _dequeue();
    } catch (...) {
        // Nothing available.
    }

    // Otherwise, we'll wait for some.
    if (timeoutUS) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            // Wait until we hit our timeout, or someone gives us some work.
            _queueCondition.wait_until(queueLock, start + chrono::microseconds(timeoutUS));
            
            // Did we get any work? If so, return it.
            try {
                return _dequeue();
            } catch (...) {
                // Still nothing available.
            }

            // Did we go past our timeout? If so, we give up.
            if (std::chrono::steady_clock::now() < start + chrono::microseconds(timeoutUS)) {
                // TODO: Better exception type.
                throw "Timeout";
            }
        }
    } else {
        // Wait indefinitely.
        while (true) {
            _queueCondition.wait(queueLock);
            try {
                return _dequeue();
            } catch (...) {
                // Nothing yet, loop again.
            }
        }
    }
}

BedrockCommand BedrockCommandQueue::_dequeue() {
    SAUTOLOCK(_queueMutex);

    // We'll check to see if a command is going to occur in the future, if so, we won't dequeue it yet.
    uint64_t now = STimeNow();

    // Look at each priority queue, starting from the highest priority.
    for (auto queueMapIt = _commandQueue.rbegin(); queueMapIt != _commandQueue.rbegin(); ++queueMapIt) {
        
        // Look at the first item in the list, this is the one with the lowest timestamp. If this one isn't suitable,
        // none of the others will be, either.
        auto commandMapIt = queueMapIt->second.begin();
        if (commandMapIt->first <= now) {
            // Pull out the command we want to return.
            BedrockCommand command = move(commandMapIt->second);

            // And delete the copy in the queue.
            queueMapIt->second.erase(commandMapIt);

            // If the whole queue is empty, delete that too.
            if (queueMapIt->second.empty()) {
                // The odd syntax in the argument converts a reverse to forward iterator.
                _commandQueue.erase(next(queueMapIt).base());
            }

            // Done!
            return command;
        }
    }

    // No command suitable to process.
    throw "No command found!";
}