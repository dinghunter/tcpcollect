#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <pcap.h>
#include <pcap/sll.h>
#include <time.h>
#include <assert.h>

#include "log.h"
#include "packet.h"
#include "address.h"
#include "mysqlpcap.h"
#include "protocol.h"
#include "hash.h"
#include "adlist.h"

#define likely(x)   __builtin_expect(!!(x), 1) 
#define unlikely(x) __builtin_expect(!!(x), 0) 


#define VALUE_SIZE 1024

/*
    if dev has set, use it, else use 'any'
    if dev not exists use 'any'

*/

list *L;

struct OrderPacket {
    uint    tcp_seq;
    char*  payload;
    size_t  payload_len;
};

int findPacket(void *ptr, void *tcp_seq) {
    struct OrderPacket* op = ptr;
    uint ts = (*(uint*)tcp_seq);
    if (op->tcp_seq == ts) {
        return 1; 
    } else {
        return 0; 
    }
}

int FindPacket(char **data, size_t *datalen, uint* handle_seq);

int SaveFindOrder(uint receive_seq, char **data, size_t *datalen, uint* handle_seq);

int
start_packet(MysqlPcap *mp) {

    L = listCreate();
    L->match = findPacket;

    struct bpf_program fcode;
    char filter[256];
    char ebuf[PCAP_ERRBUF_SIZE];

    mp->pd = pcap_open_live(mp->netDev, CAP_LEN, 0, 0, ebuf);
              
    if (NULL == mp->pd) {
        dump(L_WARN, "pcap_open_live warn: %s - %s", mp->netDev, ebuf);
              
        snprintf(mp->netDev, sizeof(mp->netDev), "%s", "any");
        mp->pd = pcap_open_live(mp->netDev, CAP_LEN, 0, 0, ebuf);
              
        if (NULL == mp->pd) {
            dump(L_ERR, "pcap_open_live error: %s - %s", "any", ebuf);
            return ERR;
        }
    }
              
    if (pcap_lookupnet(mp->netDev, &mp->localnet, &mp->netmask, ebuf) < 0) {
        dump(L_ERR, "pcap_open_live error: %s - %s", mp->netDev, ebuf);
        return ERR;
    }         
              
    dump(L_OK, "Listen Device is %s", mp->netDev);

    //snprintf(filter, sizeof(filter), "tcp port %d ", mp->mysqlPort);

    snprintf(filter, sizeof(filter), 
        "tcp port %d and tcp[tcpflags] & (tcp-push|tcp-ack) != 0", mp->mysqlPort);

    /* 8m */
    pcap_set_buffer_size(mp->pd, 1024 * 1024 * 16);

    if (pcap_compile(mp->pd, &fcode, filter, 0, mp->netmask) < 0) {
        dump(L_ERR, "pcap_compile failed: %s", pcap_geterr(mp->pd));
        pcap_freecode(&fcode);
        return ERR;
    }

    if (pcap_setfilter(mp->pd, &fcode) < 0) {
        dump(L_ERR, "pcap_setfilter failed: %s", pcap_geterr(mp->pd));
        pcap_freecode(&fcode);
        return ERR;
    }

    pcap_freecode(&fcode);

    dump(L_OK, "%-20.20s%-16.16s%-10.10s%-10.10s%s", "timestamp", "latency(us)", "rows", "user", "sql");
    dump(L_OK, "%-20.20s%-16.16s%-10.10s%-10.10s%s", "---------", "-----------", "----", "----", "---");

    pcap_loop(mp->pd, -1, process_packet, (u_char *)mp);

    dump(L_ERR, "pcap_open_live error: %s - %s", mp->netDev, ebuf);
              
    pcap_close(mp->pd);

    return OK;
}

void
process_packet(unsigned char *user, const struct pcap_pkthdr *header,
        const unsigned char *packet) {

    MysqlPcap *mp = (MysqlPcap *) user;
    struct pcap_stat ps;
    pcap_stats(mp->pd, &ps);
    //printf("recv:%u-drop:%u-ifdrop:%u\n", ps.ps_recv, ps.ps_drop, ps.ps_ifdrop);

    const struct sll_header *sll;
    const struct ether_header *ether_header;
    const struct ip *ip;
    unsigned short packet_type;

    // Parse packet
    switch (pcap_datalink(mp->pd)) {
        
    case DLT_LINUX_SLL: // any come here
        sll = (struct sll_header *) packet;
        packet_type = ntohs(sll->sll_protocol);
        ip = (const struct ip *) (packet + sizeof(struct sll_header));
        
        break;
        
    case DLT_EN10MB:
        ether_header = (struct ether_header *) packet;
        packet_type = ntohs(ether_header->ether_type);
        ip = (const struct ip *) (packet + sizeof(struct ether_header));
        
        break;
        
    case DLT_RAW:
        packet_type = ETHERTYPE_IP; //This is raw ip
        ip = (const struct ip *) packet;
        
        break;
        
    default:
        
        return;
        
    }
    
    if (packet_type != ETHERTYPE_IP)
        return;
    
    process_ip(mp, ip, header->ts);
}

int
process_ip(MysqlPcap *mp, const struct ip *ip, struct timeval tv) {

    char src[16], dst[16], *addr;
    int incoming;
    unsigned len;
    
    addr = inet_ntoa(ip->ip_src);
    strncpy(src, addr, 15);
    src[15] = '\0';
    
    addr = inet_ntoa(ip->ip_dst);
    strncpy(dst, addr, 15);
    dst[15] = '\0';
    
    if (is_local_address(mp->al, ip->ip_src))
        incoming = 0;
    else if (is_local_address(mp->al, ip->ip_dst))
        incoming = 1;
    else
        return 1;
    
    len = htons(ip->ip_len);
    
    switch (ip->ip_p) {
        struct tcphdr *tcp;
        uint16_t sport, dport, lport, rport;
        unsigned datalen;
    
    case IPPROTO_TCP:
        tcp = (struct tcphdr *) ((unsigned char *) ip + sizeof(struct ip));
        
#if defined(__FAVOR_BSD)
        sport = ntohs(tcp->th_sport);
        dport = ntohs(tcp->th_dport);
        datalen = len - sizeof(struct ip) - tcp->th_off * 4;    // 4 bits offset 
#else
        sport = ntohs(tcp->source);
        dport = ntohs(tcp->dest);
        datalen = len - sizeof(struct ip) - tcp->doff * 4;
#endif

        //printf("---------datalen -----------------%d %u %u\n", datalen, ntohl(tcp->seq), ntohl(tcp->ack_seq));

        // Capture only "data" packets, ignore TCP control
        if (datalen == 0)
            break;

        // for loopback, dst & src are all local_address
        if (ip->ip_dst.s_addr == ip->ip_src.s_addr) { 
            if (dport == mp->mysqlPort) {
                incoming = 1; 
            } else 
                incoming = 0;
        }

        /* cmd packet & auth packet */

        /* internal 
         * receive auth, insert state = 1 (incoming = 1)
         * if state = 1 after ok packet state = 2, if error packet, remove entry (incoming = 0)
         * if state = 2 can go cmd and resultset packet
         * if state = 2 can fin, remove entry
         */

        /*
            Client                                      Server
            ==========================================================
                                                        handshake
            auth        AfterAuthPacket 
                        AfterOkPacket                   auth ok| error         

            sql         AfterSqlPacket 
                        AfterResultPacket               resultset              

            prepare     AfterPreparePacket      
                        AfterPrepareOkPacket            prepare-ok             

            execute     AfterSqlPacket
                        AfterResultPacket               resultset               

            stmt_close

        */
        char *sql, *user, *data;
        user = NULL;
        int cmd = -1;

        data = (char*) ((unsigned char *) tcp + tcp->doff * 4);

        if (incoming) {
            /* ignore remote MySQL port connect locate random port */
            if ((dport != mp->mysqlPort))
                break;

            lport = dport;
            rport = sport;
           
            if (likely(is_sql(data, datalen, &user))) {
                assert(user == NULL);

                /* COM_ packet */
                cmd = data[4];

                if (unlikely(cmd == COM_QUIT)) {

                    dump(L_DEBUG, "quit packet %s %d", sql, cmd);
                    hash_get_rem(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, NULL, NULL, NULL);

                } else if (unlikely(cmd == COM_STMT_PREPARE)) {

                    parse_sql(data, &sql, datalen);
                    assert(sql);

                    dump(L_DEBUG, "prepare packet %s %d", sql, cmd);

                    hash_set(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, tv, sql, cmd, NULL, AfterPreparePacket);

                } else if (unlikely(cmd == COM_STMT_CLOSE)) {

                    /* close param_type, param */
                    dump(L_DEBUG, "stmt close packet %s %d", sql, cmd);
                    hash_get_rem(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, NULL, NULL, NULL);

                } else if (unlikely(cmd == COM_STMT_EXECUTE)) {

                    int stmt_id;
                    uchar *param_type = NULL;
                    uchar param[VALUE_SIZE];
                    param[0] = '\0';
                    int insert_param_type = 0;
                    int param_count = -1;

                    /* stmt_id */
                    parse_stmt_id(data, datalen, &stmt_id);
                    if (stmt_id <= 0) {
                        dump(L_DEBUG, "stmt error ");
                        break;
                    }

                    /* param_count, param_type(possible) */
                    
                    hash_get_param_count(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, stmt_id, &param_count, &param_type);

                    if (param_count != -1) {
                        /* prepare cant find, param_type in payload */
                        //if (param_type == NULL)
                        //   insert_param_type = 1;

                        insert_param_type = parse_param(data, datalen, param_count, &param_type, &param[0]);

                        assert(param_type);

                        dump(L_DEBUG, "execute packet %s %d", sql, cmd);

                        hash_set_param(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                            lport, rport, tv, stmt_id, param, insert_param_type ? param_type:NULL, param_count);
                    }

                } else if (unlikely(cmd == COM_BINLOG_DUMP)) {

                    dump(L_DEBUG, "binlog dump", sql, cmd);
                    hash_set(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, tv, "binlog dump", cmd, NULL, AfterSqlPacket);
               
                } else if (likely(cmd >= 0)) {
                    /* COM_QUERY */
                    parse_sql(data, &sql, datalen);
                    assert(sql);

                    dump(L_DEBUG, "sql packet %s %d", sql, cmd);
                    hash_set(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                        lport, rport, tv, sql, cmd, NULL, AfterSqlPacket);

                } else 
                    assert(NULL);
            } else {
                assert(user);
                /* auth packet */
                hash_set(mp->hash, ip->ip_dst.s_addr, ip->ip_src.s_addr, 
                    lport, rport, tv, NULL, cmd, user, AfterAuthPacket);
                dump(L_DEBUG, "auth packet %s", user);
            }
        } else {
            /* ignore locate random port connect remote MySQL port */
            if (sport != mp->mysqlPort)
                break;

            lport = sport;
            rport = dport;

            struct timeval tv2;
            time_t tv_t;
            struct tm *tm;
            tv_t = tv.tv_sec;
            tm = localtime(&tv_t);

            char tt[16];
            uchar **lastData = NULL;
            size_t *lastDataSize = NULL;
            ulong *lastNum = NULL;
            char *value = NULL;
            unsigned int *tcp_seq = NULL;

            /* TODO other hash_set must clear lastNum lastData, lastDataSize */
            int status = hash_get(mp->hash, ip->ip_src.s_addr, ip->ip_dst.s_addr,
                lport, rport, &tv2, &sql, &user, &value, &lastData, 
                &lastDataSize, &lastNum, &tcp_seq);


            if (likely(AfterSqlPacket == status)) {
                if (*tcp_seq == 0) {
                    *tcp_seq =ntohl(tcp->seq) + datalen;
                    //printf("first receive packet\n");
                } else {
                    if (*tcp_seq == ntohl(tcp->seq)) {
                        //printf("continue packet expect is %u, now is %u \n", *tcp_seq, ntohl(tcp->seq)); 
                        *tcp_seq = ntohl(tcp->seq) + datalen;
                    } else {
                        printf(" error packet expect %u but %u \n", *tcp_seq , ntohl(tcp->seq)); 
                        // skip error packet 
                        if (*lastData) {
                            free(*lastData);
                            *lastData = NULL;
                        }
                        *lastDataSize = 0;
                        *lastNum = 0;
                        // fast over this packet
                        break;

                        //if (0 == SaveFindOrder(ntohl(tcp->seq), &data, &datalen, tcp_seq)) {
                        //   printf("cant find packet in L\n");
                        //    break;
                        //}
                    }
                }

                long num;
                ulong latency;

                //_FindPacktAgain_:
                    //printf("----data----lastDataSize %d\n", *lastDataSize);
                num = parse_result(data, datalen, lastData, lastDataSize, lastNum);
                assert((num == -2) || (num >= 0) || (num == -1));
                latency = (tv.tv_sec - tv2.tv_sec) * 1000000 + (tv.tv_usec - tv2.tv_usec);
                // resultset
                if (value) {
                    // prepare-statement
                    snprintf(tt, sizeof(tt), "%d:%d:%d:%ld", 
                        tm->tm_hour, tm->tm_min, tm->tm_sec, tv2.tv_usec);

                    dump(L_OK, "%-20.20s%-16ld%-10ld%-10.10s %s [%s]", tt,
                        latency, num, user, sql, value);
                } else {
                    // normal statement
                    snprintf(tt, sizeof(tt), "%d:%d:%d:%ld", 
                        tm->tm_hour, tm->tm_min, tm->tm_sec, tv2.tv_usec);

                    dump(L_OK, "%-20.20s%-16d%-10ld%-10.10s %s", tt,
                        //latency, num, user, sql);
                        latency, num, user, sql);
                }
                /* try again find next packet */
                //if (1 == FindPacket(&data, &datalen, tcp_seq)) {
                 //   goto _FindPacktAgain_; 
                //}
                //hash_print(mp->hash); 
            } else if (0 == status) {
                    dump(L_DEBUG, "handshake packet or out packet but cant find session ");
            } else if (AfterAuthPacket == status) {
                ulong state = parse_result(data, datalen, NULL, NULL, NULL);
                assert( (state == ERR) || (state == OK) );
                if (unlikely(state == ERR)) {
                    // auth error packet
                    dump(L_DEBUG, "error packet ");
                    hash_get_rem(mp->hash, ip->ip_src.s_addr, ip->ip_dst.s_addr, 
                        lport, rport, NULL, NULL, NULL);
                } else {
                    // auth ok packet
                    dump(L_DEBUG, "ok packet ");
                    hash_set(mp->hash, ip->ip_src.s_addr, ip->ip_dst.s_addr, 
                        lport, rport, tv, NULL, cmd, NULL, AfterOkPacket);
                }
            } else if (AfterPreparePacket == status) {
                    dump(L_DEBUG, "prepare ok packet ");

                    int stmt_id = 0;
                    short param_count = 0;
                    parse_prepare_ok(data, datalen, &stmt_id, &param_count);

                    assert(stmt_id > 0);
                    assert(param_count >= 0);

                    hash_set_param_count(mp->hash, ip->ip_src.s_addr, ip->ip_dst.s_addr, 
                        lport, rport, stmt_id, param_count);
            }
        }

        break;
        
    default:
        break;
    }
    
    return 0;
}

int FindPacket(char **data, size_t *datalen, uint* handle_seq) 
{
    /* search cant find */
    listNode *ln = NULL;
    if ((ln = listSearchKey(L, handle_seq))) {
        //printf("find packet %d", *handle_seq); 
        /* find packet */    
        listDelNode(L, ln);
        struct OrderPacket *op = listNodeValue(ln);
        *data = op->payload;
        *datalen = op->payload_len;
        free(op);
        /* set next need packet */
        *handle_seq = *handle_seq + *datalen;
        return 1;
    } else {
        /* packet sequence is chao, current packet save */
        return 0;
    }
}

int SaveFindOrder(uint receive_seq, char **data, size_t *datalen, uint* handle_seq) 
{
    //printf("save packet %u\n", receive_seq);
    struct OrderPacket* op = malloc(sizeof(op));
    op->tcp_seq = receive_seq;
    op->payload = malloc(*datalen);
    memcpy(op->payload, *data, *datalen);
    op->payload_len = *datalen;
    listAddNodeTail(L, op);

    return FindPacket(data, datalen, handle_seq);
}
