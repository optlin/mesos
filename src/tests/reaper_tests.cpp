/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/dispatch.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>

#include "slave/reaper.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using process::Clock;
using process::Future;

using testing::_;
using testing::DoDefault;


class MockProcessListener : public ProcessExitedListener
{
public:
  MOCK_METHOD2(processExited, void(pid_t, int));
};


// This test checks that the Reaper can monitor a non-child process.
TEST(ReaperTest, NonChildProcess)
{
  // Use pipes to determine the pid of the grand child process.
  int pipes[2];
  ASSERT_NE(-1, pipe(pipes));

  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    close(pipes[1]);

    // Get the grand child's pid via the pipe.
    ASSERT_NE(-1, read(pipes[0], &pid, sizeof(pid)));

    close(pipes[0]);
  } else {
    // In child process.
    close(pipes[0]);

    // Double fork!
    if ((pid = fork()) == -1) {
      perror("Failed to fork a grand child process");
      abort();
    }

    if (pid > 0) {
      // Still in child process.
      exit(0);
    } else {
      // In grand child process.

      // Ensure the parent process exits before we write the pid.
      while (getppid() != 1);

      pid = getpid();
      if (write(pipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }
      close(pipes[1]);
      while (true); // Keep waiting till we get a signal.
    }
  }

  LOG(INFO) << "Grand child process " << pid;

  MockProcessListener listener;

  // Spawn the listener.
  spawn(listener);

  // Spawn the reaper.
  Reaper reaper;

  // Ignore the exit of the child process.
  EXPECT_CALL(listener, processExited(_,_))
    .WillRepeatedly(DoDefault());

  reaper.addListener(listener.self());

  // Ask the reaper to monitor the grand child process.
  reaper.monitor(pid);

  // Catch the exit of the grand child process.
  Future<Nothing> processExited;
  EXPECT_CALL(listener, processExited(pid, _))
    .WillOnce(FutureSatisfy(&processExited));

  // Now kill the grand child.
  // NOTE: We send a SIGKILL here because sometimes the grand child process
  // seems to be in a hung state and not responding to SIGTERM/SIGINT.
  EXPECT_EQ(0, kill(pid, SIGKILL));

  Clock::pause();

  // Now advance time until the reaper reaps the executor.
  while (processExited.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  // Ensure the reaper notifies of the terminated process.
  AWAIT_READY(processExited);

  terminate(listener);
  wait(listener);

  Clock::resume();
}
