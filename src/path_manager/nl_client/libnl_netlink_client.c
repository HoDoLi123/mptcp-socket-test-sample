/**
 * MPTCP Socket API Test App
 * File Sender with Path Manager (Client)
 * 
 * @date	: xxxx-xx-xx(xxx)
 * @author	: Ji-Hun(INSLAB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>

#include <pthread.h>

#include <netlink/msg.h>
#include <netlink/attr.h>

#include "../../header/mptcp.h"
#include "../../header/mptcp_netlink_api.h"
#include "../token/token.h"


#define DEBUG 1






/* mptcp event recv */
void event_rx_thread(void*);

/* etc function */
int get_fsize(FILE*);


/**
 * 기존의 TCP Client는 { socket() -> connect() -> recv(), send() -> close() }순서로 흘러간다.
 * 여기서 TCP Socket을 MPTCP Socket으로 설정하기 위해서는 socket()과 connect()사이에 setsockopt()을 사용한다.
 **/
int main(int argc, char** argv)
{
	char* ADDR;
	int PORT;
	char* FILE_PATH;

	int sock;
	struct sockaddr_in addr;
	int ret;

	FILE* file;
	char send_buff[1024] = { '\0', };
	int fsize = 0, nsize = 0;

	int enable = 1;

	char* scheduler = "roundrobin";

	int manager_num;
	char* manager;

	if(argc != 4){
		fprintf(stderr, "usage: %s [host_address] [port_number] [file_path]\n", argv[0]);
		return -1;
	}
	ADDR = argv[1];
	PORT = atoi(argv[2]);
	FILE_PATH = argv[3];

	/* 테스트할 Path Manager 선택 */
	printf("------------------\n");
	printf("| default    | 0 |\n");
	printf("| netlink    | 1 |\n");
	printf("------------------\n");
	while(1){
		printf("Input the Manager Number >> ");
		scanf("%d", &manager_num);
	
		if(manager_num < 0 || manager_num > 1) 
			fprintf(stderr, "usage: 0~4\n");
		else
			break;
	}

	/* 선택된 Path Manager 설정 */
	switch(manager_num){
		case 0:
			manager = "default";
			break;
		case 1:
			manager = "netlink";
			break;
	}

	/* create socket */
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock < 0){
		perror("[client] socket() ");
		return -1;
	}

	/* setsockopt()함수와 MPTCP_ENABLED(=42)상수를 사용하여 MPTCP Socket으로 Setup */
	ret = setsockopt(sock, SOL_TCP, MPTCP_ENABLED, &enable, sizeof(int));
	if(ret < 0){
		perror("[client] setsockopt(MPTCP_ENABLED) ");
		return -1;
	}

	/* setsockopt()함수와 MPTCP_PATH_MANAGER(=44)상수를 사용하여 MPTCP의 Path Manager 변경 */
	ret = setsockopt(sock, SOL_TCP, MPTCP_PATH_MANAGER, manager, strlen(manager));
	if(ret < 0){
		perror("[client] setsockopt(MPTCP_PATH_MANAGER) ");
		return -1;
	}

	/* setsockopt()함수와 MPTCP_SCHEDULER(=43)상수를 사용하여 MPTCP의Packet Scheduler 변경 */
	ret = setsockopt(sock, SOL_TCP, MPTCP_SCHEDULER, scheduler, strlen(scheduler));
	if(ret < 0){
		perror("[client] setsockopt(MPTCP_SCHEDULER) ");
		return -1;
	}

	/**
	 * libnl generic netlink api init
	 */
        struct nl_sock* nlsock = NULL;
	struct nl_msg* msg = NULL;

        int family_id, group_id;
	int check = 0;

        nlsock = nl_socket_alloc();
        if(!nlsock) {
                perror("nl_socket_alloc() ");
                return -1;
        }

        if(genl_connect(nlsock)) {
                perror("genl_connect() ");
		nl_socket_free(nlsock);
                return -1;
        }

	/**
	 * lookup mptcp genl family id
	 */
        family_id = genl_ctrl_resolve(nlsock, MPTCP_GENL_NAME);
        if(family_id < 0) {
                perror("genl_ctrl_resolve() ");
		nl_socket_free(nlsock);
                return -1;
        }

	/**
	 * lookup mptcp genl EVENT group id
	 */
        group_id = genl_ctrl_resolve_grp(nlsock, MPTCP_GENL_NAME, MPTCP_GENL_EV_GRP_NAME);
        if(group_id < 0) {
                perror("genl_ctrl_resolve_grp() ");
		nl_socket_free(nlsock);
                return -1;
        }

#if DEBUG
	printf("[main] nl_sock->fd : %d\n", nl_socket_get_fd(nlsock));
	printf("[main] family_id : %d\n", family_id);
	printf("[main] group_id : %d\n", group_id);
#endif

	/**
	 * join group
	 */
	ret = nl_socket_add_membership(nlsock, group_id);
	if(ret < 0 ){
		perror("nl_sock_add_membership() ");
		return -1;
	}

	/**
	 * thread init
	 */
	pthread_t p_thread[1];
	int th_id;
	int status;

	/**
	 * Start the thread
	 */
	th_id = pthread_create(&p_thread[0], NULL, (void*)event_rx_thread, (void*)nlsock);
	sleep(1);




	/**
	 * MPTCP Connection init (MP_CAPABLE)
	 */
	memset(&addr, 0x00, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(ADDR);
	addr.sin_port = htons(PORT);

	ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
	if(ret < 0) {
		perror("[client] connect() ");
		return -1;
	}
	printf("[client] connected\n");



	/**
	 * netlink api send message to kernel
	 *
	 * @cmd : MPTCP_CMD_ANNOUNCE
	 *
	 * @attr : token	local token
	 * @attr : loc_id	local address id
	 * @attr : family	family(AF_INET or AF_INET6)
	 * @attr : saddr	local ip
	 */
	uint32_t token = get_token(1);
	uint8_t loc_id = 1;
	uint16_t family = AF_INET;
	uint32_t saddr = inet_addr("192.168.1.10");

	msg = nlmsg_alloc();
	if(!msg) {
		perror("nlmsg_alloc() ");
		nl_socket_free(nlsock);
		return -1;
	}


	if(!genlmsg_put(msg, getpid(), NL_AUTO_SEQ, family_id, 0, 
				NLM_F_REQUEST, MPTCP_CMD_ANNOUNCE, MPTCP_GENL_VER)) {
		perror("genlmsg_put() ");
		nl_socket_free(nlsock);
		return -1;
	}

	check += nla_put(msg, MPTCP_ATTR_TOKEN, sizeof(token), &token);
	check += nla_put(msg, MPTCP_ATTR_LOC_ID, sizeof(loc_id), &loc_id);
	check += nla_put(msg, MPTCP_ATTR_FAMILY, sizeof(family), &family);
	check += nla_put(msg, MPTCP_ATTR_SADDR4, sizeof(saddr), &saddr);

	if(check < 0) {
		perror("nla_put() ");
		nlmsg_free(msg);
		nl_socket_free(nlsock);
		return -1;
	}
	check = 0;

	ret = nl_send_auto(nlsock, msg);
	if(ret < 0) {
		perror("nl_send_auto() ");
		nlmsg_free(msg);
		nl_socket_free(nlsock);
		return -1;
	}










	/**
	 * Sending data
	 */
	file = fopen(FILE_PATH, "rb");
	if(file == NULL){
		perror("[client] fopen() ");
		return -1;
	}

	fsize = get_fsize(file);

	printf("[client] file sending...(%s) %dB\n", FILE_PATH, fsize);
	while(nsize!=fsize){
		int fpsize = fread(send_buff, 1, 1024, file);
		nsize += fpsize;

		send(sock, send_buff, fpsize, 0);
	}




	/**
	 * close
	 */	
	pthread_join(p_thread[0], (void**)&status);
	fclose(file);
	nl_socket_free(nlsock);
	close(sock);

	return 0;
}

void event_rx_thread(void *arg)
{	
	int ret, cnt = 0;
	char buff[128];
	struct nl_sock *nlsock = (struct nl_sock*)arg;

	int fd = nl_socket_get_fd(nlsock);
#if DEBUG
	printf("[thread] nlsock->fd : %d\n", fd);
#endif
	while(1){
		cnt++;

		memset(buff, 0, sizeof(buff));
		ret = recv(fd, buff, sizeof(buff), 0);
		if(ret < 0){
			perror("recv() ");
			break;
		}

		printf("[packet num : %d]\n", cnt);
		for(int i=0; i<sizeof(buff); i++) {
			printf("%02X ", buff[i]);
		}
		printf("\n\n");
	}
}





int get_fsize(FILE* file)
{
	int fsize;

	fseek(file, 0, SEEK_END);
	fsize=ftell(file);
	fseek(file, 0, SEEK_SET);	

	return fsize;
}