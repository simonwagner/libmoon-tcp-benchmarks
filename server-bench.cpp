/*
Taken and adapted from https://github.com/xdecroc/epollServ
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>
#include <map>
#include <chrono>
#include <iostream>
#include <future>
#include <thread>

#include <sched.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <tbb/concurrent_unordered_map.h>

#include "cxxopts.hpp"

#define BUFFERSIZE 512
#define MAXEVENTS 2000

static const std::chrono::high_resolution_clock::time_point EPOCH = std::chrono::high_resolution_clock::now();
std::mutex io_mutex;

pid_t 
Gettid()
{
	return syscall(__NR_gettid);
}

enum ReportFormat {
    HUMAN = 0,
    MACHINE = 1,
};

struct client_data {
    std::chrono::high_resolution_clock::time_point lastReportTimepoint;
    std::chrono::high_resolution_clock::time_point connectedTimepoint;
    size_t lastReportBytesSend;
    int id;
    
    void connected(int id) {
        reset();
        this->id = id; 
        connectedTimepoint = std::chrono::high_resolution_clock::now();
    };
    
    void reset() {
        lastReportBytesSend = 0;
        lastReportTimepoint = std::chrono::high_resolution_clock::now();
    };
    
    void didReadBytes(size_t count) {
        lastReportBytesSend += count;
    };
    
    void maybeReport(ReportFormat format=HUMAN, size_t connections = 1) {
        auto now = std::chrono::high_resolution_clock::now();
        if(now - lastReportTimepoint >= std::chrono::seconds(5)) {
            report(format, connections);
        }
    }
    
    void report(ReportFormat format=HUMAN, size_t connections = 1) {
        std::lock_guard<std::mutex> lk(io_mutex);
        
        auto now = std::chrono::high_resolution_clock::now();
        if(format == HUMAN) {
            auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(now - lastReportTimepoint);
            double speed = (lastReportBytesSend/duration.count()) / 1e6 * 8.0;
            
            if(connections > 1) {
                std::cerr << "["<< Gettid() << ":" << id << "] Speed: " << speed / connections << "Mb/s/connection" << "\n";
            }
            else {
                std::cerr << "["<< Gettid() << ":" <<  id << "] Speed: " << speed << "Mb/s" << "\n";
            }
        }
        else {
            auto nowSinceEpoch = std::chrono::duration_cast<std::chrono::duration<double>>(now - EPOCH);
            auto lastReportSinceEpoch = std::chrono::duration_cast<std::chrono::duration<double>>(lastReportTimepoint - EPOCH);
            std::cerr << Gettid() << ";" << id << ";"  << lastReportBytesSend << ";" << connections << ";" << lastReportSinceEpoch.count() << ";" << nowSinceEpoch.count() << "\n";
        }

        reset();
    }
};

using namespace std;

int 
GetNumCPUs() 
{
	return sysconf(_SC_NPROCESSORS_ONLN);
}

int 
core_affinitize(int cpu)
{
    cpu_set_t cpus;
    int n = GetNumCPUs();

	if (cpu < 0 || cpu >= (int) n) {
		errno = -EINVAL;
		return -1;
	}

	CPU_ZERO(&cpus);
	CPU_SET((unsigned)cpu, &cpus);

	int ret = sched_setaffinity(Gettid(), sizeof(cpus), &cpus);
    
    return ret;
}

/* Simple single threaded server 
 * utilising epoll I/O event notification mechanism 
 *
 * compile: g++ epollServ.c -o _epoll
 * Usage: _epoll <port>
 * clients connect using telnet localhost <port>
**/

int serverSock_init (unsigned short port)
{
  int sfd;
  int status;
  struct sockaddr_in serv_addr = {0};
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  sfd = socket (AF_INET, SOCK_STREAM, 0); // create endpoint socketFD
  if (sfd == -1) {
    fprintf (stderr, "Socket error\n");
    close (sfd); 
    return -1;
  }

  int optval = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)); // set port reuse opt

  status = bind(sfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)); // bind addr to sfd, addr in this case is INADDR_ANY 
  if (status == -1)
  {
    fprintf(stderr, "Could not bind\n");
    return -1;      
  }  

  return sfd;
}

#define SUM_SENTIL 0

int epoll_main(unsigned short port, ReportFormat format, bool reportOnlySum, int limit);

int main (int argc, char *argv[])
{
    ReportFormat format = ReportFormat::HUMAN;
    bool reportOnlySum = false;
    int limit = -1;

    cxxopts::Options options("server-bench", "epoll server benchmark");

    options.add_options()
      ("machine", "Enable machine readable output")
      ("human", "Enable human readable output (default)")
      ("sum-only", "Print only the summed up throughput of all connections")
      ("limit", "Print only the summed up throughput of all connections", cxxopts::value<int>()->default_value("-1"))
      ("port", "Port to listen on", cxxopts::value<int>())
      ;
    options.parse_positional("port");
    
    options.parse(argc, argv);
    
    if(options.count("machine")) {
        format = ReportFormat::MACHINE;
    }
    else if(options.count("human")) {
        format = ReportFormat::HUMAN;
    }
    
    if(options.count("sum-only")) {
        reportOnlySum = true;
    }
    
    limit = options["limit"].as<int>();
    
    unsigned short port = (unsigned short)options["port"].as<int>();
    
    vector<future<int>> futures;
    for(int i = 0; i < GetNumCPUs(); i++) {
        future<int> future = async(std::launch::async, [i, port, format, reportOnlySum, limit]{ 
            core_affinitize(i);
            return epoll_main(port, format, reportOnlySum, limit);
        });
        futures.emplace_back(move(future));
    }
    
    for(const auto& future: futures) {
        future.wait();
    }
}

int epoll_main(unsigned short port, ReportFormat format, bool reportOnlySum, int limit)
{
  int sfd, s, efd;
  struct epoll_event event;
  struct epoll_event *events;
  tbb::concurrent_unordered_map<int,client_data> clientMap;
  bool shutdownOnNextClose = false;
  
  clientMap[SUM_SENTIL] = client_data();
  clientMap[SUM_SENTIL].connected(SUM_SENTIL);

  sfd = serverSock_init (port); 
  if (sfd == -1)
    abort ();

  int flags = fcntl (sfd, F_GETFL, 0);  // change socket fd to be non-blocking
  flags |= O_NONBLOCK;
  fcntl (sfd, F_SETFL, flags);

  s = listen (sfd, SOMAXCONN);  // mark socket as passive socket type 
  if (s == -1)
    {
      perror ("listen");
      abort ();
    }

  efd = epoll_create1 (0);  // create epoll instance 
  if (efd == -1)
    {
      perror ("epoll_create");
      abort ();
    }

  event.data.fd = sfd;
  event.events = EPOLLIN | EPOLLET;  // just interested in read's events using edge triggered mode
  s = epoll_ctl (efd, EPOLL_CTL_ADD, sfd, &event); // Add server socket FD to epoll's watched list
  if (s == -1)
    {
      perror ("epoll_ctl");
      abort ();
    }

  /* Events buffer used by epoll_wait to list triggered events */
  events = (epoll_event*) calloc (MAXEVENTS, sizeof(event));  

  /* The event loop */
  while (1)
    {
      int n, i;

      n = epoll_wait (efd, events, MAXEVENTS, -1);  // Block until some events happen, no timeout
      for (i = 0; i < n; i++)
	{
	  
           /* Error handling */
           if ((events[i].events & EPOLLERR) ||
              (events[i].events & EPOLLHUP) ||
              (!(events[i].events & EPOLLIN)))
	    {
              /* An error has occured on this fd, or the socket is not
                 ready for reading (why were we notified then?) */
                int       error = 0;
                socklen_t errlen = sizeof(error);
                if (getsockopt(events[i].data.fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0)
                {
                    printf("epoll error = %s (%d)\n", strerror(error), error);
                }
                else {
                    fprintf (stderr, "epoll error\n");
                }
    	      close (events[i].data.fd);  // Closing the fd removes from the epoll monitored list
                  clientMap.unsafe_erase(events[i].data.fd);
    	      continue;

            
	    }

	    /* serverSocket accepting new connections */
            else if (sfd == events[i].data.fd)
	    {
              /* We have a notification on the listening socket, which
                 means one or more incoming connections. */
              while (1)
                {
                  struct sockaddr in_addr;
                  socklen_t in_len;
                  int infd;
                  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

                  in_len = sizeof in_addr;
                  infd = accept (sfd, &in_addr, &in_len); // create new socket fd from pending listening socket queue
                  if (infd == -1) // error
                    {
                      if ((errno == EAGAIN) ||
                          (errno == EWOULDBLOCK))
                        {
                          /* We have processed all incoming connections. */
                          break;
                        }
                      else
                        {
                          perror ("accept");
                          break;
                        }
                    }

                  int optval = 1;
                  setsockopt(infd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));  // set socket for port reuse
                  
                  /* get the client's IP addr and port num */
                  s = getnameinfo (&in_addr, in_len,
                                   hbuf, sizeof hbuf,
                                   sbuf, sizeof sbuf,
                                   NI_NUMERICHOST | NI_NUMERICSERV);
                  if (s == 0)
                    {
                      printf("Accepted connection on descriptor %d "
                             "(host=%s, port=%s)\n", infd, hbuf, sbuf);
                    }

                  /* Make the incoming socket non-blocking and add it to the
                     list of fds to monitor. */        
                  int flags = fcntl (infd, F_GETFL, 0);
                  flags |= O_NONBLOCK;
                  fcntl (infd, F_SETFL, flags);

                  event.data.fd = infd;
                  event.events = EPOLLIN | EPOLLET;                  

                  s = epoll_ctl (efd, EPOLL_CTL_ADD, infd, &event); 
                  if (s == -1)
                    {
                      perror ("epoll_ctl");
                      abort ();
                    }
                client_data connected_client_data;
                connected_client_data.connected(event.data.fd);
                connected_client_data.reset();
                clientMap[event.data.fd] = connected_client_data;  // init msg counter
                if(clientMap.size() - 1 >= limit) {
                    shutdownOnNextClose = true;
                }
                }
              continue;
            }
          else
            {
              /* We have data on the fd waiting to be read. Read and
                 count it. We must read whatever data is available
                 completely, as we are running in edge-triggered mode
                 and won't get a notification again for the same
                 data. */
              int done = 0;
              int sumCount = 0;

              while (1)
                {
                  ssize_t count;
                  char buf[BUFFERSIZE];

                  count = read (events[i].data.fd, buf, sizeof buf);  
                  
                  if (count == -1)
                    {
                      /* If errno == EAGAIN, that means we have read all
                         data. So go back to the main loop. */
                      if (errno != EAGAIN)
                        {
                          perror ("read");
                          done = 1;
                        }
                      break;
                    }
                  else if (count == 0)
                    {
                      /* End of file. The remote has closed the
                         connection. */
                      done = 1;
                      break;
                    }

                    sumCount += count;
                }
                // Increment msg counter
              if(!reportOnlySum) {
                  clientMap[events[i].data.fd].didReadBytes(sumCount);
                  clientMap[events[i].data.fd].maybeReport(format);
              }
              else {
                  clientMap[SUM_SENTIL].didReadBytes(sumCount);
                  clientMap[SUM_SENTIL].maybeReport(format, clientMap.size() - 1);
              }
              
              if (done)
                {
                  printf ("Closed connection on descriptor %d\n",
                          events[i].data.fd);

                  /* Closing the descriptor will make epoll remove it
                     from the set of descriptors which are monitored. */
                  close (events[i].data.fd);
                  clientMap.unsafe_erase(events[i].data.fd);
                }
            }
        }
    }

  free (events);
  close (sfd);

  return EXIT_SUCCESS;
}
