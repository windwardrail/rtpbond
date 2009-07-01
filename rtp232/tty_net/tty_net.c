/*
name:			tty_net.c
author(s):		Iomsn Egenson
version:		0.14
description:	Creates a virtual tty device which communicates over UDP. Uses FIFOs.
history:		0.13:	- sends to a specific port and receives from it
						- creates CDP and RTP header
						- extracts CDP and RTP header
						- sends a "Im alive" package when time to last package exceeds TMax
						- adjustable remote IP, remote port, TMax and baud rate
						- multi threaded
						- uses FIFOs as virtual devices
				0.14:	- user guide was commented out for use with netserial
						- uses local sockets to communicate with netserial
						- improved error handling
*/
		
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
		
#define PL_SIZE 150
#define RTP_HEADER_SIZE 12
#define CDP_HEADER_SIZE 10
#define MAX_SN 65535
		
#define GET_CONF 1
#define CLOSE_DEV 2

static char *help="usage: tty_net deviceName destIp4Addr destPort listenPort (0 for any) baudRate[bytes/s] TMax[ms] [debugMode]\n";

// contains info about the device
struct netSerDev
{
	char name[256];
	char destIp4Addr[15];	// IPv4
	int destPort;
	int listenPort;
	int baudRate;
	int TMax;
} devConfig;

struct soRtpHeader_t
{
	uint8_t version;
		// + version    : 2 
		// + padding    : 1
		// + extension  : 1
		// + CRSC Count : 4
	uint8_t type;
	uint16_t sequence;  // sequence number
	uint32_t timestamp;
	uint32_t ssrc;      // synchonizsation source identifier
};
  
struct cdpHeader_t
{
	uint8_t length;
	uint8_t reserved;
	uint64_t timestamp;
};
		
struct parameters
{
	long int baudrate;
	double Tmax;
};
		
		
int socketSend, socketRecv;
struct sockaddr_in destAddress;	// destination address for sending
struct sockaddr_in localAddress;	// local address
int destAddrLength;
struct parameters params;	// sending parameters
char devName[256];	// name of the virtual device
uint32_t startTime;	// time whe program is started (used as SSRC)
char debugMode = 0;	// 1 for debug mode (send time out package with size > 0)


// appends the RTP header to bufferCdp and writes the result in bufferCdpRtp
void appendRtpHeader(char* bufferCdp, struct soRtpHeader_t RtpHeader, char* bufferCdpRtp)
{
	int i, arrayPos;
		
	// write the RtpHeader in bufferCdpRtp
	bufferCdpRtp[0] = RtpHeader.version;
	bufferCdpRtp[1] = RtpHeader.type;
	arrayPos = 2;
	for (i = arrayPos; i < sizeof(uint16_t) + arrayPos; i++)
		bufferCdpRtp[sizeof(uint16_t)+2*arrayPos-i-1] = RtpHeader.sequence >> 8*(i-arrayPos);
	arrayPos = i;				
	for (i = arrayPos; i < sizeof(uint32_t) + arrayPos; i++)
		bufferCdpRtp[sizeof(uint32_t)+2*arrayPos-i-1] = RtpHeader.timestamp >> 8*(i-arrayPos);
	arrayPos = i;
	for (i = arrayPos; i < sizeof(uint32_t) + arrayPos; i++)
		bufferCdpRtp[sizeof(uint32_t)+2*arrayPos-i-1] = RtpHeader.ssrc >> 8*(i-arrayPos);
	// write the payload + cdpHeader in bufferCdpRtp
	for (i = 0; i < CDP_HEADER_SIZE + PL_SIZE; i++)
		bufferCdpRtp[i+RTP_HEADER_SIZE] = bufferCdp[i];
	
	return bufferCdpRtp;
}
		

// appends the CDP header to buffer and writes the result in bufferCdp
void appendCdpHeader(char* buffer, struct cdpHeader_t CdpHeader, char* bufferCdp)
{
	int i, arrayPos;
	
	// write the CdpHeader in bufferCdp
	bufferCdp[0] = CdpHeader.length;
	bufferCdp[1] = CdpHeader.reserved;
	arrayPos = 2;
	for (i = arrayPos; i < sizeof(uint64_t)+arrayPos; i++)
		bufferCdp[sizeof(uint64_t)+2*arrayPos-i-1] = CdpHeader.timestamp >> 8*(i-arrayPos);				
	// write the payload in bufferCdp
	for (i = 0; i < PL_SIZE; i++)
		bufferCdp[i+CDP_HEADER_SIZE] = buffer[i];
	
	return bufferCdp;
}
		
		
struct soRtpHeader_t retreiveRtpHeader(char *bufferCdpRtp)
{
	struct soRtpHeader_t RtpHeader;
	
	RtpHeader.version = bufferCdpRtp[0];
	RtpHeader.type = bufferCdpRtp[1];
	RtpHeader.sequence = bufferCdpRtp[2]*256 + bufferCdpRtp[3];
	RtpHeader.timestamp = bufferCdpRtp[4]*256*256*256 + bufferCdpRtp[5]*256*256 + bufferCdpRtp[6]*256 + bufferCdpRtp[7];
	
	return RtpHeader;
}
		
		
struct cdpHeader_t retreiveCdpHeader(char *bufferCdpRtp)
{
	struct cdpHeader_t CdpHeader;
	int i;
	
	CdpHeader.length = bufferCdpRtp[RTP_HEADER_SIZE];
	CdpHeader.reserved = bufferCdpRtp[RTP_HEADER_SIZE+1];
	CdpHeader.timestamp = 0;
	for (i = 0; i < sizeof(uint64_t); i++)
		CdpHeader.timestamp += bufferCdpRtp[RTP_HEADER_SIZE+1+sizeof(uint64_t)-i] = 256*i;
	
	return CdpHeader;
}
		

// exit handler which closes sockets/FIFOs
void closeDev(void *arg)
{
	int *fd = (int *)arg;
	
	// close socket/FIFO
	close(*fd);
}


// exit handler which removes sockets/FIFOs
void removeDev(void *arg)
{
	char *name = (char *)arg;
	
	// remove socket/FIFO
	unlink(name);
}


void * recvRtp()
{
	char buffer[PL_SIZE];	// payload
	char bufferCdpRtp[PL_SIZE + CDP_HEADER_SIZE + RTP_HEADER_SIZE];	// payload + cdpHeder + rtpHeader		
	int err;
	int inFifo;	// write payload in fifo
	struct cdpHeader_t cdpHeader;
	struct soRtpHeader_t rtpHeader;
	struct sockaddr_in remoteAddress;	// remote address
	unsigned int remoteAddrLength;
	int i;
	char inFifoName[256+4];
	
	// exit handlers
	pthread_cleanup_push(removeDev, (void *)inFifoName);
	pthread_cleanup_push(closeDev, (void *)&inFifo);
	pthread_cleanup_push(closeDev, (void *)&socketRecv);
	
	strcpy(inFifoName, devName);
	strncat(inFifoName, ".in", 3);
	// set up FIFO.in
	inFifo = open(inFifoName, O_RDWR);
	if (inFifo < 0)
	{
		// create FIFO
		err = mkfifo(inFifoName, 0666);
		if(err < 0)
		{
			printf("tty_net: error: cannot create %s\n", inFifoName);
			exit(1);
		}
	}
    
	// bind socket to the local address
	err = bind(socketRecv, (struct sockaddr *) &localAddress, sizeof(localAddress));
    if ((err) < 0)
	{		
        printf("tty_net: error: bind(socketRecv) failed\n");
		exit(1);
	}
	
	// receive data from the net and write it into FIFO
	while(1)
	{
		remoteAddrLength = sizeof(remoteAddress);
		// clear buffer
		memset(bufferCdpRtp, 0, PL_SIZE + RTP_HEADER_SIZE + CDP_HEADER_SIZE);
		
		// retreive data from socket
		err = recvfrom(socketRecv, bufferCdpRtp, PL_SIZE + CDP_HEADER_SIZE + RTP_HEADER_SIZE, 0, (struct sockaddress *) &remoteAddress, &remoteAddrLength);
				
		if (err > 0)
		{		
			// split off the RtpHeader
			rtpHeader = retreiveRtpHeader(bufferCdpRtp);
			// split off the CdpHeader
			cdpHeader = retreiveCdpHeader(bufferCdpRtp);			
			// split off the payload
			for (i = 0; i < PL_SIZE; i++)
				buffer[i] = bufferCdpRtp[i + CDP_HEADER_SIZE + RTP_HEADER_SIZE];
			
			printf("rtpHeader.type = %i\n", rtpHeader.type);
			// write into FIFO
			// if (rtpHeader.type == 77)	// incoming data
				write(inFifo, buffer, PL_SIZE);
		}			
		if (err < 0)
			printf("tty_net: error: cannnot read from socket\n");
	}
	
	close(inFifo);
	close(socketRecv);
	
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);	
	pthread_cleanup_pop(1);	
	return 0;
}
		
		
void * sendRtp()
{
	char buffer[PL_SIZE];	// payload
	char* bufferCdp = malloc(PL_SIZE + CDP_HEADER_SIZE);	// payload + cdpHeader
	char* bufferCdpRtp = malloc(PL_SIZE + CDP_HEADER_SIZE + RTP_HEADER_SIZE);	// payload + cdpHeader + rtpHeader
	int err;
	int outFifo;	// read the payload from here
	struct cdpHeader_t cdpHeader;
	struct soRtpHeader_t rtpHeader;
	int sn;	// sequence number
	clock_t tLastSent = 0;	// time when last data package was sent
	char outFifoName[256+4];
	
	// exit handlers
	pthread_cleanup_push(removeDev, (void *)&outFifoName);
	pthread_cleanup_push(closeDev, (void *)&outFifo);
	pthread_cleanup_push(closeDev, (void *)&socketSend);
	
	strcpy(outFifoName, devName);
	strncat(outFifoName, ".out", 4);
	// set up outFifo
	err = mkfifo(outFifoName, 0666);
	outFifo = open(outFifoName, O_RDONLY);
	if (outFifo < 0)
	{
		printf("tty_net: error: cannot open FIFO %s\n", outFifoName);
		exit(1);
	}
	
	sn = 0;
	// empty the buffers
	memset(buffer, 0, PL_SIZE);
	memset(bufferCdp, 0, CDP_HEADER_SIZE + PL_SIZE);
	memset(bufferCdpRtp, 0, RTP_HEADER_SIZE + CDP_HEADER_SIZE + PL_SIZE);
	
	while(1)
	{		
		// read data from FIFO
		err = read(outFifo, buffer, PL_SIZE);
		if (err < 0)
		{
			printf("tty_net: error: cannot read from FIFO.out\n");
			return(-1);
		}
		
		// send package when we got real data or send empty package after timeout Tmax
		if ((err > 0) || difftime(clock(), tLastSent)/CLOCKS_PER_SEC >= params.Tmax/1000.0)
		{		
			// make rtp header
			rtpHeader.version = 2*64 + 0*32;	// Version 2 + Padding 0
			rtpHeader.type = 77;				// for data
			rtpHeader.sequence = sn;			// sequence number
			rtpHeader.timestamp = sn*PL_SIZE;
			
			// make cdp header
			cdpHeader.length = strlen(buffer);
			cdpHeader.timestamp = difftime(clock(),startTime);
			cdpHeader.timestamp = cdpHeader.timestamp << 4*8;
			
			if (sn == MAX_SN)
				sn = 0;
			else
				sn++;
			
			// in case of timeout send "Im alive"			
			if (err == 0) {
				buffer[0] = 'I'; buffer[1] = 'm'; buffer[2] = ' '; buffer[3] = 'a';
				buffer[4] = 'l'; buffer[5] = 'i'; buffer[6] = 'v'; buffer[7] = 'e';
				cdpHeader.length = 0;
				if (debugMode == 1)
					cdpHeader.length = strlen(buffer);
			}
									 
			// append headers Pay attention to the order!
			appendCdpHeader(buffer, cdpHeader, bufferCdp);
			appendRtpHeader(bufferCdp, rtpHeader, bufferCdpRtp);
		
			// wait if last package was sent too recently (baud rate)
			while (difftime(clock(), tLastSent)/CLOCKS_PER_SEC <= (double) PL_SIZE/params.baudrate)
			{
				 // wait
			}
			
			// send data through socket
			err = sendto(socketSend, bufferCdpRtp, RTP_HEADER_SIZE + CDP_HEADER_SIZE + PL_SIZE, 0, (struct sockaddress *) &destAddress, destAddrLength);
			if (err < 0)
			{
				printf("error: cannot send data\n");
				exit(1);
			}
			
			tLastSent = clock();
			
			// empty the buffer
			memset(buffer, 0, PL_SIZE);			
		}		
	}
	
	close(outFifo);
	close(socketSend);
	
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);	
	pthread_cleanup_pop(1);
	return 0;
}
		


int main(int argc, char *argv[])
{
	pthread_t threads[2];	// receive and send thread
	int err;
	
	char cmd[1];	// command from netserial ((re)start, close, getConf)
	int socketCmd, socketCfg;	// sockets to communicate with netserial (kind of IPC)
	struct sockaddr_un cmdAddress, cfgAddress;
	unsigned int cmdAddressLength, cfgAddressLength;
	
	if (argc < 7){
		printf(help);
		return -1;
	}
	
	startTime = clock();
	srand(time(NULL));	// random seed numbers for the ssrc
	// rtpHeader.ssrc = random();	// the SSRC 32bit iedentifies the sender
	
	// printf("=== tty_net ===\n\n");
				
	// device name of FIFO
	strcpy(devName, argv[1]);
			
	// address structure of the destination
	destAddress.sin_family = AF_INET;
	destAddress.sin_addr.s_addr = inet_addr(argv[2]);
	destAddress.sin_port = htons(atoi(argv[3]));
	destAddrLength = sizeof(destAddress);

    // construct local address structure
    memset(&localAddress, 0, sizeof(localAddress));		// zero out structure
    localAddress.sin_family = AF_INET;					// internet address family
    localAddress.sin_addr.s_addr = htonl(INADDR_ANY);	// any incoming interface
    localAddress.sin_port = htons(atoi(argv[4]));		// listen port (0 to accept from any port)
	
	// sending parameters
	params.baudrate = atoi(argv[5]);
	params.Tmax = atoi(argv[6]);
	if (argc == 8)
		debugMode = atoi(argv[7]);
			
	// create internet sockets with UDP protocol
	socketSend = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	socketRecv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	if ((socketSend < 0) || (socketRecv < 0))
	{
		printf("tty_net: error: creating socket(s) failed\n");
		exit(1);
	}
				
	// receiving thread
	err = pthread_create(&threads[0], NULL, recvRtp, NULL);
	if (err)
	{
		printf("tty_net: error: creating recvRtp failed, return code from pthread_create() is %d\n", err);
		exit(1);
	}
	// sending thread
	err = pthread_create(&threads[1], NULL, sendRtp, NULL);
	if (err)
	{
		printf("tty_net: error: creating sendRtp failed, return code from pthread_create() is %d\n", err);
		exit(1);
	}
				
	// create local sockets for communication with netserial
	if ((socketCmd = socket(AF_LOCAL, SOCK_DGRAM, 0)) == -1)
	{
		printf("tty_net: creating socketCmd failed\n");
		exit(1);
	}
	if ((socketCfg = socket(AF_LOCAL, SOCK_DGRAM, 0)) == -1)
	{
		printf("tty_net: creating socketCfg failed\n");
		exit(1);
	}
	
	cmdAddress.sun_family = AF_LOCAL;
	strcpy(cmdAddress.sun_path, devName);
	strncat(cmdAddress.sun_path, ".cmd", 4);
	unlink(cmdAddress.sun_path);
	cmdAddressLength = strlen(cmdAddress.sun_path) + sizeof(cmdAddress.sun_family);
	
	if (bind(socketCmd, (struct sockaddr *)&cmdAddress, cmdAddressLength) == -1)
	{
		printf("tty_net: error: binding local socket failed\n");
		exit(1);
	}
	
	cfgAddressLength = sizeof(cfgAddress);
		
	// waiting for command from netserial
	cmd[0] = 0;
	while (cmd[0] == 0)
	{
		if (recvfrom(socketCmd, cmd, 1, 0, (struct sockaddress *) &cfgAddress, &cfgAddressLength) < 0)
		{
			printf("tty_net: error: receiving from local socket failed\n");
			exit(1);
		}
		
		if (cmd[0] == GET_CONF)
		{
			// printf("tty_net: sending config of %s\n", devName);
			// write info in a struct
			strcpy(devConfig.name, devName);
			strcpy(devConfig.destIp4Addr, argv[2]);
			devConfig.destPort = atoi(argv[3]);
			devConfig.listenPort = atoi(argv[4]);
			devConfig.baudRate = params.baudrate;
			devConfig.TMax = params.Tmax;
						
			cfgAddress.sun_family = AF_LOCAL;
			strcpy(cfgAddress.sun_path, devName);
			strncat(cfgAddress.sun_path, ".cfg", 4);
			cfgAddressLength = sizeof(cfgAddress);
			
			// send config to netserial
			sendto(socketCfg, (struct netSerDev *) &devConfig, sizeof(devConfig), 0, (struct sockaddress *) &cfgAddress, cfgAddressLength);
			cmd[0] = 0;
		}
		else if (cmd[0] == CLOSE_DEV)
		{
			// printf("tty_net: closing %s\n", devName);
			// close everything
			pthread_cancel(threads[0]);
			pthread_cancel(threads[1]);
			close(socketCmd);
			unlink(cmdAddress.sun_path);
		}
	}
	
		
	pthread_exit(NULL);	
	return 0;
}
