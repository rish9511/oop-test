#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>

#if defined(__linux__)
#include "client/linux/crash_generation/client_info.h"
#include "client/linux/crash_generation/crash_generation_server.h"
#elif defined(__APPLE__)
#include "client/mac/crash_generation/client_info.h"
#include "client/mac/crash_generation/crash_generation_server.h"
#include "common/mac/MachIPC.h"
#endif

using google_breakpad::ClientInfo;
using google_breakpad::CrashGenerationServer;
#if defined(__APPLE__)
using google_breakpad::ReceivePort;
using google_breakpad::MachPortSender;
using google_breakpad::MachReceiveMessage;
using google_breakpad::MachSendMessage;
#endif
using std::string;

#if defined(__linux__) || defined(__APPLE__)
pthread_mutex_t mutex     = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  condition_var   = PTHREAD_COND_INITIALIZER;
#endif

#if defined(__linux__)
void OnChildProcessDumpRequested(void* aContext,
                                 const ClientInfo* aClientInfo,
                                 const string* aFilePath)
#elif defined(__APPLE__)
void OnChildProcessDumpRequested(void* aContext,
                                 const ClientInfo& aClientInfo,
                                 const string& aFilePath)
#endif
{
  pid_t pid =
#if defined(__linux__)
    aClientInfo->pid();
#elif defined(__APPLE__)
  aClientInfo.pid();
#endif
  const char* dump_path =
#if defined(__linux__)
    aFilePath->c_str();
#elif defined(__APPLE__)
    aFilePath.c_str();
#endif
  printf("handler: wrote dump for client %d: %s\n", pid, dump_path);
#if defined(__linux__) || defined(__APPLE__)
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&condition_var);
  pthread_mutex_unlock(&mutex);
#endif
}

int main(int argc, char** argv)
{
  printf("handler: starting\n");
  const int required_args =
#if defined(__linux__)
    3;
#elif defined(__APPLE__)
    2;
#endif
  if (argc < required_args) {
    fprintf(stderr,
            "usage: handler: <pipe fd> <server fd>\n");
    return 1;
  }
#if defined(__linux__) || defined(__APPLE__)
  int pipe_fd = atoi(argv[1]);
#if defined(__linux__)
  int server_fd = atoi(argv[2]);
#endif
  pthread_mutex_lock(&mutex);
#endif

  string dump_path = "/home/g_rishabh/test_programs/mozilla_code/dumps";
#if defined(__linux__)
  CrashGenerationServer crash_server(
    server_fd,
    OnChildProcessDumpRequested, NULL,
    NULL, NULL,                 // we don't care about process exit here
    true,
    &dump_path);
#elif defined(__APPLE__)
  // Use the bootstrap port, which the parent process has set, to
  // send a message to the parent process.
  mach_port_t bootstrap_port;
  if (task_get_bootstrap_port(mach_task_self(),
                              &bootstrap_port) != KERN_SUCCESS) {
    printf("handler: failed to get bootstrap port\n");
    return 1;
  }
  
  ReceivePort receiver;
  MachPortSender sender(bootstrap_port);
  MachSendMessage send_message(0);
  // Include a port to send a reply on.
  send_message.AddDescriptor(receiver.GetPort());
  sender.SendMessage(send_message, MACH_MSG_TIMEOUT_NONE);
  printf("handler: sent message to parent\n");

  // Now wait for a reply.
  MachReceiveMessage receive_message;
  if (receiver.WaitForMessage(&receive_message,
                              MACH_MSG_TIMEOUT_NONE) != KERN_SUCCESS) {
    printf("handler: failed to get a reply from parent\n");
    return 1;
  }
  printf("handler: got message from parent with %d descriptors\n",
         receive_message.GetDescriptorCount());

  // Restore the bootstrap port to the original that the parent just sent.
  if (task_set_bootstrap_port(mach_task_self(),
                              receive_message.GetTranslatedPort(0)) != KERN_SUCCESS) {
    printf("handler: failed to reset bootstrap port\n");
    return 1;
  }

  // Now use the prearranged port to start the crash server.
  CrashGenerationServer crash_server(
    receive_message.GetTranslatedPort(1),
    OnChildProcessDumpRequested, NULL,
    NULL, NULL,                 // we don't care about process exit here
    true,
    dump_path);
#endif

  if (!crash_server.Start()) {
    fprintf(stderr, "handler: Failed to start CrashGenerationServer\n");
    return 1;
  }
  printf("handler: started server\n");

#if defined(__linux__) || defined(__APPLE__)
  // Signal parent that this process has started the server.
  uint8_t byte = 1;
  write(pipe_fd, &byte, sizeof(byte));

  printf("handler: waiting for client request\n");
  pthread_cond_wait(&condition_var, &mutex);
#endif

  printf("handler: shutting down\n");
  crash_server.Stop();
  printf("handler: exiting\n");

  return 0;
}
