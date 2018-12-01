/*
 * Copyright 2018 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <poll.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include "wallClock.h"
#include "os.h"
#include "profiler.h"
#include "stackFrame.h"


const int THREADS_PER_TICK = 7;

long WallClock::_interval;

void WallClock::signalHandler(int signo, siginfo_t* siginfo, void* ucontext) {
    Profiler::_instance.recordSample(ucontext, _interval, 0, NULL);
}

Error WallClock::start(Arguments& args) {
    if (args._interval < 0) {
        return Error("interval must be positive");
    }
    _interval = args._interval ? args._interval : DEFAULT_INTERVAL;

    OS::installSignalHandler(SIGPROF, signalHandler);

    if (pipe(_pipefd) != 0) {
        return Error("Unable to create poll pipe");
    }

    if (pthread_create(&_thread, NULL, threadEntry, this) != 0) {
        close(_pipefd[1]);
        close(_pipefd[0]);
        return Error("Unable to create timer thread");
    }

    return Error::OK;
}

void WallClock::stop() {
    char val = 1;
    ssize_t r = write(_pipefd[1], &val, sizeof(val));
    (void)r;

    close(_pipefd[1]);
    pthread_join(_thread, NULL);
    close(_pipefd[0]);
}

void WallClock::timerLoop() {
    ThreadList* thread_list = NULL;

    int self = OS::threadId();
    struct pollfd fds = {_pipefd[0], POLLIN, 0};
    int timeout = _interval > 1000000 ? (int)(_interval / 1000000) : 1;

    while (poll(&fds, 1, timeout) == 0) {
        if (thread_list == NULL) {
            thread_list = OS::listThreads();
        }

        for (int i = 0; i < THREADS_PER_TICK; i++) {
            int thread_id = thread_list->next();
            if (thread_id == -1) {
                delete thread_list;
                thread_list = NULL;
                break;
            } else if (thread_id != self) {
                OS::sendSignalToThread(thread_id, SIGPROF);
            }
        }
    }

    delete thread_list;
}

int WallClock::getCallChain(void* ucontext, int tid, const void** callchain, int max_depth,
                            const void* jit_min_address, const void* jit_max_address) {
    StackFrame frame(ucontext);
    const void* pc = (const void*)frame.pc();
    uintptr_t fp = frame.fp();
    uintptr_t prev_fp = (uintptr_t)&fp;

    int depth = 0;
    const void* const valid_pc = (const void*)0x1000;

    // Walk until the bottom of the stack or until the first Java frame
    while (depth < max_depth && pc >= valid_pc && !(pc >= jit_min_address && pc < jit_max_address)) {
        callchain[depth++] = pc;

        // Check if the next frame is below on the current stack
        if (fp <= prev_fp || fp >= prev_fp + 0x40000) {
            break;
        }

        prev_fp = fp;
        pc = ((const void**)fp)[1];
        fp = ((uintptr_t*)fp)[0];
    }

    return depth;
}