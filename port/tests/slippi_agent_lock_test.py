"""Compile the account lock and verify it is reached before the network entry points."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

import fixture_compiler

ROOT = Path(__file__).resolve().parents[2]
CXX = fixture_compiler.require_cxx()

class SlippiAgentLockTest(unittest.TestCase):
    def compile(self, name, code):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / name
            source = '#include "slippi_agent_lock.h"\n' + code
            subprocess.run([*CXX, '-std=c++17', '-pthread',
                '-I', str(ROOT/'port/runtime/hle'), '-x', 'c++', '-', '-o', str(exe)],
                input=source, text=True, check=True)
            env = os.environ.copy()
            for key in ('MELEE_AGENT_COPY','YOUGAME_AGENT_COPY','YOUGAME_TEST_LABEL'):
                env.pop(key, None)
            subprocess.run([str(exe)], env=env, check=True)

    def test_environment_and_once_per_operation(self):
        self.compile('env', r'''
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <set>
std::mutex mutex; std::vector<std::string> lines;
namespace host { void log(const char* fmt, ...) {
 char line[128]; va_list ap; va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);
 std::lock_guard<std::mutex> lock(mutex);lines.emplace_back(line);
} }
#define CHECK(x) do { if(!(x))std::abort(); } while(0)
int main() {
 using namespace host::test_env;
 for (const char* flag : {"MELEE_AGENT_COPY","YOUGAME_AGENT_COPY"}) {
   unsetenv(flag); CHECK(!agent_copy()); setenv(flag,"1",1); CHECK(agent_copy());
   setenv(flag,"0",1); CHECK(!agent_copy()); unsetenv(flag);
 }
 setenv("YOUGAME_TEST_LABEL","fixture",1);CHECK(agent_copy());unsetenv("YOUGAME_TEST_LABEL");
 using namespace slippi::agent_lock;
 for(int i=0;i<int(Operation::Count);++i) CHECK(!blocked(Operation(i)));
 CHECK(lines.empty());
 setenv("MELEE_AGENT_COPY","1",1);
 std::vector<std::thread> threads;
 for(int t=0;t<8;++t)threads.emplace_back([] {
  for(int n=0;n<20;++n)for(int i=0;i<int(Operation::Count);++i)CHECK(blocked(Operation(i)));
 });
 for(auto& t:threads)t.join();
 std::set<std::string> expected;
 for(const char* what : {"login","server refresh","version check","matchmaking","matchmaking peers",
                        "http","spectator","account linking","game reporting"})
  expected.insert(std::string("agent test: slippi locked (")+what+")");
 CHECK(lines.size()==expected.size());CHECK(std::set<std::string>(lines.begin(),lines.end())==expected);
 puts("agent environment and concurrent once-only logs passed");
}
''')

    def test_network_entry_points_check_the_lock(self):
        def before(path, function, guard, action):
            s = (ROOT/'port/runtime/hle'/path).read_text().split(function, 1)[1]
            self.assertLess(s.index(guard), s.index(action), function)
        before('slippi_net.cpp', 'bool User::AttemptLogin', 'Operation::Login', 'account::resolve')
        before('slippi_net.cpp', 'std::string Matchmaking::openSlippiTicket',
               'Operation::Matchmaking', 'enet_host_create')
        before('slippi_spectate.cpp', 'void start()', 'Operation::Spectator', 'std::thread')
        before('slippi_report.cpp', 'void log_game(', 'Operation::Reporting', 'g_queue.push_back')

if __name__ == '__main__':
    unittest.main()
