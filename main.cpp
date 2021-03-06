#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

#include <netinet/ip.h>
#include <netinet/tcp.h>
#include<string.h>

bool ishost=false;
char *HOST;
char PREFIX[]="Host: ";
char *FOUND;
//char *http_order[6]={"GET","POST","HEAD","PUT","DELETE","OPTIONS"};
bool found_hostdotdot(unsigned char *buf,int size){
	if(memcmp(buf,"GET",strlen("GET"))&&
	memcmp(buf,"POST",strlen("POST"))&&
	memcmp(buf,"HEAD",strlen("HEAD"))&&
	memcmp(buf,"PUT",strlen("PUT"))&&
	memcmp(buf,"DELETE",strlen("DELETE"))&&
	memcmp(buf,"OPTIONS",strlen("OPTIONS"))
	){return false;}
		
	int l=strlen(PREFIX);
	for(int i=0;i<size;i++){
		if(i+l>size)break;
		if(memcmp(buf+i,PREFIX,l)==0){
			buf+=i+l;
			int cpylen=0;
			while(cpylen+i+l<size&&buf[cpylen]!='\r'||buf[cpylen+1]!='\n'){cpylen++;}
			buf[cpylen++]='\0';
			FOUND=(char *)buf;
			return true;
		}
	}
	return false;
}
void check_host(unsigned char* buf, int size) {
	struct ip *IP=(struct ip *)buf;
	if(IP->ip_p==IPPROTO_TCP){
		buf+=IP->ip_hl*4;
		size-=IP->ip_hl*4;

		struct tcphdr *TCP=(struct tcphdr *)buf;
		size-=TCP->doff*4;
		buf+=TCP->doff*4;

		if(found_hostdotdot(buf,size)){
			if(strcmp(HOST,FOUND)==0)
				ishost=true;
		}
	}
}	

/* returns packet id */
static u_int32_t print_pkt (struct nfq_data *tb)
{
	ishost=false;
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	u_int32_t mark,ifi; 
	int ret;
	unsigned char *data;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
		printf("hw_protocol=0x%04x hook=%u id=%u ",
			ntohs(ph->hw_protocol), ph->hook, id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);

		printf("hw_src_addr=");
		for (i = 0; i < hlen-1; i++)
			printf("%02x:", hwph->hw_addr[i]);
		printf("%02x ", hwph->hw_addr[hlen-1]);
	}

	mark = nfq_get_nfmark(tb);
	if (mark)
		printf("mark=%u ", mark);

	ifi = nfq_get_indev(tb);
	if (ifi)
		printf("indev=%u ", ifi);

	ifi = nfq_get_outdev(tb);
	if (ifi)
		printf("outdev=%u ", ifi);
	ifi = nfq_get_physindev(tb);
	if (ifi)
		printf("physindev=%u ", ifi);

	ifi = nfq_get_physoutdev(tb);
	if (ifi)
		printf("physoutdev=%u ", ifi);

	ret = nfq_get_payload(tb, &data);
	if (ret >= 0){
		printf("payload_len=%d ", ret);
		check_host(data,ret);
	}
	
	fputc('\n', stdout);

	return id;
}
	

static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *data)
{
	u_int32_t id = print_pkt(nfa);
	printf("entering callback\n");
	if(ishost){
		printf("I found host! And dropped it!\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}
	return nfq_set_verdict(qh,id,NF_ACCEPT,0,NULL);
}
void usage(){
	printf("syntax : netfilter_block <host>\n");
	printf("sample : netfilter_block test.gilgil.net\n");
	exit(-1);
}
int main(int argc, char **argv)
{
	if(argc!=2) usage();
	HOST=argv[1];

	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	struct nfnl_handle *nh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));

	printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	printf("binding this socket to queue '0'\n");
	qh = nfq_create_queue(h,  0, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}
		/* if your application is too slow to digest the packets that
		 * are sent from kernel-space, the socket buffer that we use
		 * to enqueue packets may fill up returning ENOBUFS. Depending
		 * on your application, this error may be ignored. nfq_nlmsg_verdict_putPlease, see
		 * the doxygen documentation of this library on how to improve
		 * this situation.
		 */
		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	exit(0);
}

