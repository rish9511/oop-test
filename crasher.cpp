#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/limits.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/socket.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <poll.h>
#endif

#include <iostream>
#include <string>

#if defined(__linux__)
#include "client/linux/crash_generation/crash_generation_server.h"
#include "client/linux/handler/exception_handler.h"
#include "common/linux/eintr_wrapper.h"
#include "common/linux/linux_libc_support.h"
#include "third_party/lss/linux_syscall_support.h"
#elif defined(__APPLE__)
#include "client/mac/crash_generation/crash_generation_server.h"
#include "client/mac/handler/exception_handler.h"
#include "common/mac/MachIPC.h"
#define HANDLE_EINTR(x) { x }
#endif

using google_breakpad::CrashGenerationServer;
using std::string;

static google_breakpad::ExceptionHandler* eh = 0;
static google_breakpad::MinidumpDescriptor* md = 0;

#if defined(__linux__)
int server_fd, client_fd;
#elif defined(__APPLE__)
using google_breakpad::ReceivePort;
using google_breakpad::MachPortSender;
using google_breakpad::MachMsgPortDescriptor;
using google_breakpad::MachReceiveMessage;
using google_breakpad::MachSendMessage;
mach_port_t handler_port = MACH_PORT_NULL;
#endif
const char* handler_path_str;

void crashme()
{
  volatile int* x = (int*)42;
  *x = 1;
}

bool FilterCallback(void *context)
{
//    std::cerr << "inside the call back " << __FUNCTION__ << std::endl <<  "  " <<  static_cast<google_breakpad::ExceptionHandler::CrashContext* >(context)->tid<<  std::endl;
//    std::cerr << std::endl << "pid of crashing process " << static_cast<google_breakpad::ExceptionHandler::CrashContext* >(context)->tid<<  std::endl;
  printf("crasher: in FilterCallback\n");
  // Launch handler
#if defined(__linux__) || defined(__APPLE__)
  int fds[2];
  if (pipe(fds) == -1) {
    printf("crasher: pipe failed!\n");
    return false;
  }

#if defined(__APPLE__)
  // Swap the bootstrap port for a different port, so we can
  // use the replacement port to communicate with the child.
  ReceivePort receiver;
  // Hold on to the existing bootstrap port.
  mach_port_t bootstrap_port;
  if (task_get_bootstrap_port(mach_task_self(),
                              &bootstrap_port) != KERN_SUCCESS)
    return false;
  
  if (task_set_bootstrap_port(mach_task_self(),
                              receiver.GetPort()) != KERN_SUCCESS)
    return false;
#endif
  pid_t handler_pid = fork();
  if (handler_pid == 0) {
    // In child process.
    //close(fds[0]);

    //printf("crasher: in child after fork\n");
    // Pass the pipe fd and server fd as arguments.
    char pipe_fd_string[8];
    sprintf(pipe_fd_string, "%d", fds[1]);
#if defined(__linux__)
    char server_fd_string[8];
    sprintf(server_fd_string, "%d", server_fd);
#endif
    char* const argv[] = {
      strdup(handler_path_str),
      pipe_fd_string,
#if defined(__linux__)
      server_fd_string,
#endif
      NULL
    };
    execv(handler_path_str,
          argv);
    printf("crasher: execv failed\n");
    exit(1);
  }
#if defined(__APPLE__)
  // Reset bootstrap port.
  if (task_set_bootstrap_port(mach_task_self(),
                              bootstrap_port) != KERN_SUCCESS)
    return false;
  // Wait for child to return a port on which to perform messaging.
  MachReceiveMessage receive_message;
  if (receiver.WaitForMessage(&receive_message,
                              MACH_MSG_TIMEOUT_NONE) != KERN_SUCCESS)
    return false;
  printf("crasher: got message from child with %d descriptors\n",
         receive_message.GetDescriptorCount());
  // Now send back the prearranged port to use, as well as the original
  // bootstrap port;
  MachPortSender sender(receive_message.GetTranslatedPort(0));
  MachSendMessage send_message(0);
  if (!send_message.AddDescriptor(bootstrap_port)) {
    printf("crasher: failed to add bootstrap port\n");
    return false;
  }
  if (!send_message.AddDescriptor(MachMsgPortDescriptor(handler_port,
                                                        MACH_MSG_TYPE_MOVE_RECEIVE))) {
    printf("crasher: failed to add handler port\n");
    return false;
  }
  if (sender.SendMessage(send_message, MACH_MSG_TIMEOUT_NONE) != KERN_SUCCESS) {
    printf("crasher: SendMessage failed\n");
    return false;
  }
  printf("crasher: sent reply message\n");
#endif
  //close(fds[1]);
  // Wait for handler to unblock us.
  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fds[0];
  pfd.events = POLLIN | POLLERR;

  int r = HANDLE_EINTR(poll(&pfd, 1, 5000));
  if (r != 1 || (pfd.revents & POLLIN) != POLLIN) {
    printf("crasher: poll failed?\n");
    if (pfd.revents & POLLERR) {
      printf("crasher: POLLERR\n");
    }
    if (pfd.revents & POLLHUP) {
      printf("crasher: POLLHUP\n");
    }
    if (pfd.revents & POLLNVAL) {
      printf("crasher: POLLNVAL\n");
    }
    return false;
  }
  uint8_t junk;
  ssize_t _size = read(fds[0], &junk, sizeof(junk));

  std::cout << "received from server: " << _size << std::endl;
  close(fds[0]);
  // use the socket to send the crash data to server

  using google_breakpad::ExceptionHandler;
//  ExceptionHandler::CrashContext* _cc = reinterpret_cast<ExceptionHandler::CrashContext*>(context);
//  std::cout << "pid of crashing process : " << _cc << std::endl;

  static const unsigned kControlMsgSize =
      CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred));
  char control[kControlMsgSize];
  struct iovec _iovec[1];
  _iovec[0].iov_base = context;
  _iovec[0].iov_len = sizeof(google_breakpad::ExceptionHandler::CrashContext);
//  _iovec->iov_base = context;
//  _iovec->iov_len = sizeof(google_breakpad::ExceptionHandler::CrashContext);




  struct msghdr crash_info;
  crash_info.msg_flags = 0;
  crash_info.msg_name = nullptr;
  crash_info.msg_namelen = 0;
  crash_info.msg_iov = _iovec;
  crash_info.msg_iovlen = 1;
  crash_info.msg_control = nullptr;
  crash_info.msg_controllen = 0;


  ssize_t _len = sendmsg(server_fd, &crash_info, 0);


  if(_len != -1 )
    std::cout << "numbers of characters sent : " << _len << std::endl;

  else
      std::cout << "error : " << strerror(errno) << std::endl;


#endif
  printf("crasher: exiting FilterCallback\n");

  return true;
}


static string GetHandlerPath()
{
  // Locate handler binary next to the current binary.
  char self_path[PATH_MAX];
#if defined(__linux__)
  if (readlink("/proc/self/exe", self_path, sizeof(self_path) - 1) == -1) {
    printf("crasher: readlink failed: %s", strerror(errno));
    return "";
  }
#elif defined(__APPLE__)
  uint32_t size = sizeof(self_path) - 1;
  if (_NSGetExecutablePath(self_path,  &size) != 0) {
    printf("crasher: NSGetExecutablePath failed\n");
    return "";
  }
#endif
  self_path[sizeof(self_path) - 1] = '\0';
  
  string handler_path(self_path);
  size_t pos = handler_path.rfind('/');
  if (pos == string::npos) {
    printf("crasher: can't locate handler\n");
    return "";
  }
  handler_path.erase(pos + 1);
  handler_path += "handler";
  return handler_path;
}


int main(int argc, char** argv)
{
  printf("crasher: started\n");

  string handler_path = GetHandlerPath();
  handler_path_str = handler_path.c_str();

#if defined(__linux__)
  // Setup client/server sockets
  if (!CrashGenerationServer::CreateReportChannel(&server_fd,
                                                  &client_fd)) {
    printf("crasher: CreateReportChannel failed!\n");
    return 1;
  }
#elif defined(__APPLE__)
  // We'll send this to the handler using Mach messaging via the
  // bootstrap port.
  ReceivePort receive;
  handler_port = receive.GetPort();
#endif

#if defined(__linux__)
  google_breakpad::MinidumpDescriptor* md = new  google_breakpad::MinidumpDescriptor("/home/g_rishabh/test_programs/mozilla_code/dumps");
  google_breakpad::ExceptionHandler handler(*md,
                           FilterCallback,
                           NULL,    // no minXdump callback
                           NULL,    // no callback context
                           true,    // install signal handlers
                           client_fd);
//    md  = new google_breakpad::MinidumpDescriptor("/home/g_rishabh/test_programs/mozilla_code/dumps");
//    eh =  new google_breakpad::ExceptionHandler(*md, 0, FilterCallback,0,true, -1);

#elif defined(__APPLE__)
  google_breakpad::ExceptionHandler handler(
                           FilterCallback,
                           NULL,    // no callback context
                           true,    // install signal handlers
                           handler_port
                           );
#endif
  printf("crasher: about to crash\n");
  crashme();
  return 0;
}
