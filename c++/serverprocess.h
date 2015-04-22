#ifndef _RELAY_SERVERPROCESS_H
#define _RELAY_SERVERPROCESS_H

#include <atomic>
#include <condition_variable>
#include <list>

#include "mruset.h"

#define DISCONNECT_STARTED 1
#define DISCONNECT_PRINT_AND_CLOSE 2
#define DISCONNECT_FROM_WRITE_THREAD 4
#define DISCONNECT_FROM_READ_THREAD 8
#define DISCONNECT_COMPLETE 16

#define SERVER_DECLARE_CLASS_VARS \
private: \
	const int sock; \
	std::mutex send_mutex; \
	std::atomic<int> connected; \
 \
	std::condition_variable cv; \
	std::list<std::shared_ptr<std::vector<unsigned char> > > outbound_secondary_queue; \
	std::list<std::shared_ptr<std::vector<unsigned char> > > outbound_primary_queue; \
	mruset<std::vector<unsigned char> > txnAlreadySeen; \
	mruset<std::vector<unsigned char> > blocksAlreadySeen; \
	bool initial_outbound_throttle; \
	uint32_t total_waiting_size; \
 \
	std::thread *read_thread, *write_thread; \
 \
public: \
	const std::string host; \
	std::atomic<int> disconnectFlags;

#define SERVER_DECLARE_CONSTRUCTOR_EXTENDS_AND_BODY \
	sock(sockIn), connected(0), txnAlreadySeen(100), blocksAlreadySeen(10), initial_outbound_throttle(true), total_waiting_size(0), \
	host(hostIn), disconnectFlags(0) { \
		std::lock_guard<std::mutex> lock(send_mutex); \
		read_thread = new std::thread(do_setup_and_read, this); \
		write_thread = new std::thread(do_write, this); \

#define SERVER_DECLARE_DESTRUCTOR \
	assert(DISCONNECT_COMPLETE); \
	if (disconnectFlags & DISCONNECT_FROM_WRITE_THREAD) \
		write_thread->join(); \
	else if (disconnectFlags & DISCONNECT_FROM_READ_THREAD)\
		read_thread->join(); \
	else \
		assert(!"DISCONNECT_COMPLETE set but not from either thread?"); \
	close(sock); \
	delete read_thread; \
	delete write_thread;

#define SERVER_DECLARE_FUNCTIONS(CLASS) \
private: \
	void disconnect_from_outside(const char* reason) { \
		if (disconnectFlags.fetch_or(DISCONNECT_PRINT_AND_CLOSE) & DISCONNECT_PRINT_AND_CLOSE) \
			return; \
 \
		printf("%s Disconnect: %s (%s)\n", host.c_str(), reason, strerror(errno)); \
		shutdown(sock, SHUT_RDWR); \
	} \
 \
	void disconnect(const char* reason) { \
		if (disconnectFlags.fetch_or(DISCONNECT_STARTED) & DISCONNECT_STARTED) \
			return; \
 \
		if (!(disconnectFlags.fetch_or(DISCONNECT_PRINT_AND_CLOSE) & DISCONNECT_PRINT_AND_CLOSE)) { \
			printf("%s Disconnect: %s (%s)\n", host.c_str(), reason, strerror(errno)); \
			shutdown(sock, SHUT_RDWR); \
		} \
 \
		if (std::this_thread::get_id() != read_thread->get_id()) { \
			disconnectFlags |= DISCONNECT_FROM_WRITE_THREAD; \
			read_thread->join(); \
		} else { \
			disconnectFlags |= DISCONNECT_FROM_READ_THREAD; \
			{ \
				/* Wake up the write thread */ \
				std::lock_guard<std::mutex> lock(send_mutex); \
				outbound_secondary_queue.push_back(std::make_shared<std::vector<unsigned char> >(1)); \
				cv.notify_all(); \
			} \
			write_thread->join(); \
		} \
 \
		outbound_secondary_queue.clear(); \
		outbound_primary_queue.clear(); \
 \
		disconnectFlags |= DISCONNECT_COMPLETE; \
	} \
 \
	static void do_setup_and_read(CLASS* me) { \
		fcntl(me->sock, F_SETFL, fcntl(me->sock, F_GETFL) & ~O_NONBLOCK); \
 \
		int nodelay = 1; \
		setsockopt(me->sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay)); \
 \
		if (errno) \
			return me->disconnect("error during connect"); \
 \
		me->net_process(); \
	} \
 \
	static void do_write(CLASS* me) { \
		me->net_write(); \
	} \
 \
	void net_write() { \
		while (true) { \
			std::shared_ptr<std::vector<unsigned char> > msg; \
			{ \
				std::unique_lock<std::mutex> write_lock(send_mutex); \
				while (!outbound_secondary_queue.size() && !outbound_primary_queue.size()) \
					cv.wait(write_lock); \
 \
				if (disconnectFlags) \
					return disconnect("disconnect started elsewhere"); \
 \
				if (outbound_primary_queue.size()) { \
					msg = outbound_primary_queue.front(); \
					outbound_primary_queue.pop_front(); \
				} else { \
					msg = outbound_secondary_queue.front(); \
					outbound_secondary_queue.pop_front(); \
				} \
 \
				total_waiting_size -= msg->size(); \
				if (!total_waiting_size) \
					initial_outbound_throttle = false; \
				else if (initial_outbound_throttle) \
					usleep(20*1000); /* Limit outbound to avg 5Mbps worst case */ \
			} \
			if (send_all(sock, (char*)&(*msg)[0], msg->size()) != int64_t(msg->size())) \
				return disconnect("failed to send msg"); \
		} \
	}

#endif
