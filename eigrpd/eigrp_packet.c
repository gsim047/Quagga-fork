/*
 * EIGRP General Sending and Receiving of EIGRP Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <zebra.h>

#include "thread.h"
#include "memory.h"
#include "linklist.h"
#include "keychain.h"
#include "prefix.h"
#include "if.h"
#include "table.h"
#include "sockunion.h"
#include "stream.h"
#include "log.h"
#include "sockopt.h"
#include "checksum.h"
#include "md5.h"
#include "sha256.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"

/* Packet Type String. */
const struct message eigrp_packet_type_str[] =
{
  { EIGRP_OPC_UPDATE,	"Update"		},
  { EIGRP_OPC_REQUEST,	"Request"		},
  { EIGRP_OPC_QUERY,	"Query"			},
  { EIGRP_OPC_REPLY,	"Reply"			},
  { EIGRP_OPC_HELLO,	"Hello"			},
  { EIGRP_OPC_IPXSAP,	"IPX-SAP"		},
  { EIGRP_OPC_PROBE,	"Probe"			},
  { EIGRP_OPC_ACK,		"Ack"			},
  { EIGRP_OPC_SIAQUERY,	"SIAQuery"		},
  { EIGRP_OPC_SIAREPLY,	"SIAReply"		},
};
const size_t eigrp_packet_type_str_max = sizeof(eigrp_packet_type_str) /
  sizeof(eigrp_packet_type_str[0]);

static unsigned char zeropad[16] = {0};

/* Forward function reference*/
static struct stream * eigrp_recv_packet (int, struct interface **, struct stream *);
static int eigrp_verify_header (struct stream *, struct eigrp_interface *, struct ip *,
				struct eigrp_header *);
static int eigrp_check_network_mask (struct eigrp_interface *, struct in_addr);


static int eigrp_retrans_count_exceeded(struct eigrp_packet *ep, struct eigrp_neighbor *nbr)
{
  return 1;
}

int
eigrp_make_md5_digest (struct eigrp_interface *ei, struct stream *s, u_char flags)
{
	struct key *key = NULL;
	struct keychain *keychain;

  unsigned char digest[EIGRP_AUTH_TYPE_MD5_LEN];
  MD5_CTX ctx;
  void *ibuf;
  size_t backup_get, backup_end;
  struct TLV_MD5_Authentication_Type *auth_TLV;
  char* cbuf;

  ibuf = s->data;
  cbuf = (char *)s->data;
  backup_end = s->endp;
  backup_get = s->getp;

  auth_TLV = eigrp_authTLV_MD5_new();

  stream_set_getp(s,EIGRP_HEADER_LEN);
  stream_get(auth_TLV,s,EIGRP_AUTH_MD5_TLV_SIZE);
  stream_set_getp(s, backup_get);

	keychain = keychain_lookup(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
	if ( keychain )
		key = key_lookup_for_send(keychain);

	memset(&ctx, 0, sizeof(ctx));
	MD5Init(&ctx);

	/* Generate a digest. Each situation needs different handling */
	if(flags & EIGRP_AUTH_BASIC_HELLO_FLAG)
	{
		if ( !key ){
			zlog_warn ("key is NULL");
			return 0;
		}
       MD5Update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
       MD5Update(&ctx, key->string, strlen(key->string));
       if(strlen(key->string) < 16)
         MD5Update(&ctx, zeropad, 16 - strlen(key->string));
     }
   else if(flags & EIGRP_AUTH_UPDATE_INIT_FLAG)
     {
       MD5Update(&ctx, ibuf, EIGRP_MD5_UPDATE_INIT_COMPUTE);
     }
	else if(flags & EIGRP_AUTH_UPDATE_FLAG)
	{
		if ( !key ){
			zlog_warn ("key is NULL");
			return 0;
		}
       MD5Update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
       MD5Update(&ctx, key->string, strlen(key->string));
       if(strlen(key->string) < 16)
         MD5Update(&ctx, zeropad, 16 - strlen(key->string));
       if(backup_end > (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE))
         {
           MD5Update(&ctx, cbuf + (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE),
               backup_end - 20 - (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE));
         }
	}

	MD5Final(digest, &ctx);

	/* Append md5 digest to the end of the stream. */
	memcpy(auth_TLV->digest,digest,EIGRP_AUTH_TYPE_MD5_LEN);

	stream_set_endp(s,EIGRP_HEADER_LEN);
	stream_put(s,auth_TLV,EIGRP_AUTH_MD5_TLV_SIZE);
	stream_set_endp(s, backup_end);

	eigrp_authTLV_MD5_free(auth_TLV);
	return EIGRP_AUTH_TYPE_MD5_LEN;
}// 


int
eigrp_check_md5_digest (struct stream *s, struct TLV_MD5_Authentication_Type *authTLV,struct eigrp_neighbor *nbr, u_char flags)
{
	MD5_CTX ctx;
	unsigned char digest[EIGRP_AUTH_TYPE_MD5_LEN];
	struct key *key = NULL;
	struct keychain *keychain;
	void *ibuf;
	char *cbuf;
	size_t backup_end;
	struct TLV_MD5_Authentication_Type *auth_TLV;
	struct eigrp_header *eigrph;


	if (nbr && ntohl(nbr->crypt_seqnum) > ntohl(authTLV->key_sequence))
	{
		zlog_warn ("interface %s: eigrp_check_md5 bad sequence %d (expect %d)",
                 IF_NAME (nbr->ei),
                 ntohl(authTLV->key_sequence),
                 ntohl(nbr->crypt_seqnum));
		return 0;
	}

	eigrph = (struct eigrp_header *) s->data;
	eigrph->checksum = 0;

	auth_TLV =(struct TLV_MD5_Authentication_Type *) (s->data + EIGRP_HEADER_LEN);
//	memcpy(auth_TLV->digest, "0", sizeof(auth_TLV->digest)); //???
	memset(auth_TLV->digest, '0', sizeof(auth_TLV->digest));

	ibuf = s->data;
	cbuf = (char *)s->data;
	backup_end = s->endp;

	keychain = keychain_lookup(IF_DEF_PARAMS (nbr->ei->ifp)->auth_keychain);
	if ( keychain )
		key = key_lookup_for_send(keychain);

	memset(&ctx, 0, sizeof(ctx));
	MD5Init(&ctx);

	/* Generate a digest. Each situation needs different handling */
	if(flags & EIGRP_AUTH_BASIC_HELLO_FLAG)
	{
		if ( !key ){
			zlog_warn ("key is NULL");
			return 0;
		}
		MD5Update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		MD5Update(&ctx, key->string, strlen(key->string));
		if ( strlen(key->string) < 16 )
			MD5Update(&ctx, zeropad, 16 - strlen(key->string));
	}
	else if(flags & EIGRP_AUTH_UPDATE_INIT_FLAG)
	{
		MD5Update(&ctx, ibuf, EIGRP_MD5_UPDATE_INIT_COMPUTE);
	}
	else if(flags & EIGRP_AUTH_UPDATE_FLAG)
	{
		if ( !key ){
			zlog_warn ("key is NULL");
			return 0;
		}
		MD5Update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		MD5Update(&ctx, key->string, strlen(key->string));
		if(strlen(key->string) < 16)
			MD5Update(&ctx, zeropad, 16 - strlen(key->string));
		if(backup_end > (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE))
		{
			MD5Update(&ctx, cbuf + (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE),
              backup_end - 20 - (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE));
		}
	}

	MD5Final(digest, &ctx);

	/* compare the two */
	if (memcmp (authTLV->digest, digest, EIGRP_AUTH_TYPE_MD5_LEN) == 0)
	{
		zlog_debug("VSETKO OK");
	}
	else
	{
		zlog_warn ("interface %s: eigrp_check_md5 checksum mismatch",
                       IF_NAME (nbr->ei));
		return 0;
	}

	/* save neighbor's crypt_seqnum */
	if (nbr)
		nbr->crypt_seqnum = authTLV->key_sequence;

	return 1;
}// 

// not used
/*
static int
strnzcpyn(char *dst, const char *src, int size)
{
        char *dptr;
        if (!size) return 0;

        dptr = dst;

        while (--size)
                if (!(*dptr++ = *src++)) return (dptr-dst)-1;
        *dptr = 0;

        return (dptr-dst)-1;
}
*/

int
eigrp_make_sha256_digest (struct eigrp_interface *ei, struct stream *s, u_char flags)
{
	struct key *key = NULL;
	struct keychain *keychain;
	char *source_ip;
//	int saved_len;
//	char saved_key[PLAINTEXT_LENGTH + 1];


	unsigned char digest[EIGRP_AUTH_TYPE_SHA256_LEN];
	unsigned char buffer[1 + PLAINTEXT_LENGTH + 45 + 1] = { 0 };
	HMAC_SHA256_CTX ctx;
	void *ibuf;
	size_t backup_get, backup_end;
	struct TLV_SHA256_Authentication_Type *auth_TLV;

	ibuf = s->data;
	backup_end = s->endp;
	backup_get = s->getp;

	auth_TLV = eigrp_authTLV_SHA256_new ();

	stream_set_getp(s,EIGRP_HEADER_LEN);
	stream_get(auth_TLV,s,EIGRP_AUTH_SHA256_TLV_SIZE);
	stream_set_getp(s, backup_get);

	keychain = keychain_lookup(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
	if (keychain)
		key = key_lookup_for_send(keychain);

//     saved_len[index] = strnzcpyn(saved_key[index], key,
//                             PLAINTEXT_LENGTH + 1);

	source_ip = calloc(16, sizeof(char));
	inet_ntop(AF_INET, &ei->address->u.prefix4, source_ip, 16);

	memset(&ctx, 0, sizeof(ctx));
	buffer[0] = '\n';
	if ( key ){
		memcpy(buffer + 1, key, strlen (key->string));
		memcpy(buffer + 1 + strlen(key->string), source_ip, strlen(source_ip));
		HMAC__SHA256_Init(&ctx, buffer, 1 + strlen (key->string) + strlen(source_ip));
		HMAC__SHA256_Update(&ctx, ibuf, strlen(ibuf));
		HMAC__SHA256_Final(digest, &ctx);
	}


	/* Put hmac-sha256 digest to it's place */
	memcpy(auth_TLV->digest,digest,EIGRP_AUTH_TYPE_SHA256_LEN);

	stream_set_endp(s,EIGRP_HEADER_LEN);
	stream_put(s,auth_TLV,EIGRP_AUTH_SHA256_TLV_SIZE);
	stream_set_endp(s, backup_end);

	eigrp_authTLV_SHA256_free(auth_TLV);
	free(source_ip);

	return EIGRP_AUTH_TYPE_SHA256_LEN;
}// eigrp_make_sha256_diges

int
eigrp_check_sha256_digest (struct stream *s, struct TLV_SHA256_Authentication_Type *authTLV,struct eigrp_neighbor *nbr, u_char flags)
{

  return 1;
}
/*
 * eigrp_packet_dump
 *
 * This routing dumps the contents of the IP packet either received or
 * built by EIGRP.
 */
static void
eigrp_packet_dump (struct stream *s)
{
    // not yet...
    return;
}

/*
 * Converts a 24-bit integer represented as an unsigned char[3] *value
 * in network byte order into uint32_t in host byte order
 */
//static uint32_t u24_32 (const unsigned char *value)
//{
//  return (value[0] << 16) + (value[1] << 8) + value[2];
//}
//
///*
// * Converts an uint32_t value in host byte order into a 24-bit integer
// * in network byte order represented by unsigned char[3] *result
// */
//static unsigned char * u32_24 (uint32_t value, unsigned char *result)
//{
//  value = htonl(value & 0x00FFFFFF);
//  memcpy (result, (unsigned char *) &value + 1, 3);
//
//  return result;
//}

int
eigrp_write (struct thread *thread)
{
  struct eigrp *eigrp = THREAD_ARG(thread);
  struct eigrp_header *eigrph;
  struct eigrp_interface *ei;
  struct eigrp_packet *ep;
  struct sockaddr_in sa_dst;
  struct ip iph;
  struct msghdr msg;
  struct iovec iov[2];
  u_int16_t opcode = 0;

  int ret;
  int flags = 0;
  struct listnode *node;
#ifdef WANT_EIGRP_WRITE_FRAGMENT
  static u_int16_t ipid = 0;
#endif /* WANT_EIGRP_WRITE_FRAGMENT */
#define EIGRP_WRITE_IPHL_SHIFT 2

  eigrp->t_write = NULL;

  node = listhead(eigrp->oi_write_q);
  assert(node);
  ei = listgetdata(node);
  assert(ei);

#ifdef WANT_EIGRP_WRITE_FRAGMENT
  /* seed ipid static with low order bits of time */
  if (ipid == 0)
  ipid = (time(NULL) & 0xffff);
#endif /* WANT_EIGRP_WRITE_FRAGMENT */

  /* Get one packet from queue. */
  ep = eigrp_fifo_head(ei->obuf);
  assert(ep);
  assert(ep->length >= EIGRP_HEADER_LEN);

  if (ep->dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS))
    eigrp_if_ipmulticast(eigrp, ei->address, ei->ifp->ifindex);

  memset(&iph, 0, sizeof(struct ip));
  memset(&sa_dst, 0, sizeof(sa_dst));

  sa_dst.sin_family = AF_INET;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
  sa_dst.sin_len = sizeof(sa_dst);
#endif /* HAVE_STRUCT_SOCKADDR_IN_SIN_LEN */
  sa_dst.sin_addr = ep->dst;
  sa_dst.sin_port = htons(0);

  /* Set DONTROUTE flag if dst is unicast. */
  if (!IN_MULTICAST(htonl(ep->dst.s_addr)))
    flags = MSG_DONTROUTE;

  iph.ip_hl = sizeof(struct ip) >> EIGRP_WRITE_IPHL_SHIFT;
  /* it'd be very strange for header to not be 4byte-word aligned but.. */
  if (sizeof(struct ip) > (unsigned int)(iph.ip_hl << EIGRP_WRITE_IPHL_SHIFT))
    iph.ip_hl++; /* we presume sizeof struct ip cant overflow ip_hl.. */

  iph.ip_v = IPVERSION;
  iph.ip_tos = IPTOS_PREC_INTERNETCONTROL;
  iph.ip_len = (iph.ip_hl << EIGRP_WRITE_IPHL_SHIFT) + ep->length;

#if defined (__DragonFly__)
  /*
   * DragonFly's raw socket expects ip_len/ip_off in network byte order.
   */
  iph.ip_len = htons(iph.ip_len);
#endif

  iph.ip_off = 0;
  iph.ip_ttl = EIGRP_IP_TTL;
  iph.ip_p = IPPROTO_EIGRPIGP;
  iph.ip_sum = 0;
  iph.ip_src.s_addr = ei->address->u.prefix4.s_addr;
  iph.ip_dst.s_addr = ep->dst.s_addr;

  memset(&msg, 0, sizeof(msg));
  msg.msg_name = (caddr_t) &sa_dst;
  msg.msg_namelen = sizeof(sa_dst);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  iov[0].iov_base = (char*)&iph;
  iov[0].iov_len = iph.ip_hl << EIGRP_WRITE_IPHL_SHIFT;
  iov[1].iov_base = STREAM_PNT(ep->s);
  iov[1].iov_len = ep->length;

  /* send final fragment (could be first) */
  sockopt_iphdrincl_swab_htosys(&iph);
  ret = sendmsg(eigrp->fd, &msg, flags);
  sockopt_iphdrincl_swab_systoh(&iph);

  if (IS_DEBUG_EIGRP_TRANSMIT(0, SEND))
    {
      eigrph = (struct eigrp_header *) STREAM_DATA(ep->s);
      opcode = eigrph->opcode;
      zlog_debug("Sending [%s] to [%s] via [%s] ret [%d].",
		 LOOKUP(eigrp_packet_type_str, opcode), inet_ntoa(ep->dst),
		 IF_NAME(ei), ret);
    }

  if (ret < 0)
    zlog_warn("*** sendmsg in eigrp_write failed to %s, "
	      "id %d, off %d, len %d, interface %s, mtu %u: %s",
	      inet_ntoa(iph.ip_dst), iph.ip_id, iph.ip_off, iph.ip_len, ei->ifp->name,
	      ei->ifp->mtu, safe_strerror(errno));

  /* Show debug sending packet. */
  if (IS_DEBUG_EIGRP_TRANSMIT(0, SEND) && (IS_DEBUG_EIGRP_TRANSMIT(0, PACKET_DETAIL)))
    {
      zlog_debug("-----------------------------------------------------");
      eigrp_ip_header_dump(&iph);
      stream_set_getp(ep->s, 0);
      eigrp_packet_dump(ep->s);
      zlog_debug("-----------------------------------------------------");
    }

  /* Now delete packet from queue. */
  eigrp_packet_delete(ei);

  if (eigrp_fifo_head(ei->obuf) == NULL)
    {
      ei->on_write_q = 0;
      list_delete_node(eigrp->oi_write_q, node);
    }

  /* If packets still remain in queue, call write thread. */
  if (!list_isempty(eigrp->oi_write_q))
    eigrp->t_write = thread_add_write(master, eigrp_write, eigrp, eigrp->fd);

  return 0;
}

/* Starting point of packet process function. */
int
eigrp_read (struct thread *thread)
{
  int ret;
  struct stream *ibuf;
  struct eigrp *eigrp;
  struct eigrp_interface *ei;
  struct ip *iph;
  struct eigrp_header *eigrph;
  struct interface *ifp;
  struct eigrp_neighbor *nbr;

  u_int16_t opcode = 0;
  u_int16_t length = 0;

  /* first of all get interface pointer. */
  eigrp = THREAD_ARG(thread);

  /* prepare for next packet. */
  eigrp->t_read = thread_add_read(master, eigrp_read, eigrp, eigrp->fd);

  stream_reset(eigrp->ibuf);
  if (!(ibuf = eigrp_recv_packet(eigrp->fd, &ifp, eigrp->ibuf)))
    {
      /* This raw packet is known to be at least as big as its IP header. */
      return -1;
    }

  /* Note that there should not be alignment problems with this assignment
   because this is at the beginning of the stream data buffer. */
  iph = (struct ip *)STREAM_DATA(ibuf);

  //Substract IPv4 header size from EIGRP Packet itself
  if(iph->ip_v == 4)
  length = (iph->ip_len) - 20U;


  /* IP Header dump. */
  if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV) && IS_DEBUG_EIGRP_TRANSMIT(0, PACKET_DETAIL))
      eigrp_ip_header_dump(iph);

  /* Note that sockopt_iphdrincl_swab_systoh was called in eigrp_recv_packet. */
  if (ifp == NULL)
    {
      /* Handle cases where the platform does not support retrieving the ifindex,
	 and also platforms (such as Solaris 8) that claim to support ifindex
	 retrieval but do not. */
      ifp = if_lookup_address(iph->ip_src);

      if (ifp == NULL)
	return 0;
    }

  /* associate packet with eigrp interface */
  ei = eigrp_if_lookup_recv_if(eigrp, iph->ip_src, ifp);

  /* eigrp_verify_header() relies on a valid "ei" and thus can be called only
     after the checks below are passed. These checks in turn access the
     fields of unverified "eigrph" structure for their own purposes and
     must remain very accurate in doing this. 
  */
  if (!ei)
    return 0;

  /* Self-originated packet should be discarded silently. */
  if (eigrp_if_lookup_by_local_addr(eigrp, NULL, iph->ip_src) ||
      (IPV4_ADDR_SAME(&iph->ip_src.s_addr, &ei->address->u.prefix4)))
    {
      if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
	zlog_debug("eigrp_read[%s]: Dropping self-originated packet",
		   inet_ntoa(iph->ip_src));
      return 0;
    }

  /* Advance from IP header to EIGRP header (iph->ip_hl has been verified
   by eigrp_recv_packet() to be correct). */

  stream_forward_getp(ibuf, (iph->ip_hl * 4));
  eigrph = (struct eigrp_header *) STREAM_PNT(ibuf);

  if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV) && IS_DEBUG_EIGRP_TRANSMIT(0, PACKET_DETAIL))
    eigrp_header_dump(eigrph);

//  if (MSG_OK != eigrp_packet_examin(eigrph, stream_get_endp(ibuf) - stream_get_getp(ibuf)))
//    return -1;

  /* Now it is safe to access all fields of EIGRP packet header. */
  /* associate packet with eigrp interface */
  ei = eigrp_if_lookup_recv_if(eigrp, iph->ip_src, ifp);

  /* eigrp_verify_header() relies on a valid "ei" and thus can be called only
    after the checks below are passed. These checks in turn access the
    fields of unverified "eigrph" structure for their own purposes and
    must remain very accurate in doing this.
   */
  if (!ei)
    return 0;

  /* If incoming interface is passive one, ignore it. */
  if (ei && EIGRP_IF_PASSIVE_STATUS(ei) == EIGRP_IF_PASSIVE)
    {
      char buf[3][INET_ADDRSTRLEN];

      if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
	zlog_debug("ignoring packet from router %s sent to %s, "
		   "received on a passive interface, %s",
		   inet_ntop(AF_INET, &eigrph->vrid, buf[0], sizeof(buf[0])),
		   inet_ntop(AF_INET, &iph->ip_dst, buf[1], sizeof(buf[1])),
		   inet_ntop(AF_INET, &ei->address->u.prefix4,
			     buf[2], sizeof(buf[2])));

      if (iph->ip_dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS))
        {
	  /* Try to fix multicast membership.
	   * Some OS:es may have problems in this area,
	   * make sure it is removed.
	   */
	  EI_MEMBER_JOINED(ei, MEMBER_ALLROUTERS);
	  eigrp_if_set_multicast(ei);
	}
      return 0;
    }

  /* else it must be a local eigrp interface, check it was received on
   * correct link
   */
  else if (ei->ifp != ifp)
    {
      if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
        zlog_warn("Packet from [%s] received on wrong link %s",
		   inet_ntoa(iph->ip_src), ifp->name);
      return 0;
    }

  /* Verify more EIGRP header fields. */
  ret = eigrp_verify_header(ibuf, ei, iph, eigrph);
  if (ret < 0)
    {
      if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
        zlog_debug("eigrp_read[%s]: Header check failed, dropping.",
                   inet_ntoa(iph->ip_src));
      return ret;
    }

  /* calcualte the eigrp packet length, and move the pounter to the
     start of the eigrp TLVs */
  opcode = eigrph->opcode;

  if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
    zlog_debug("Received [%s] length [%u] via [%s] src [%s] dst [%s]",
	       LOOKUP(eigrp_packet_type_str, opcode), length,
	       IF_NAME(ei), inet_ntoa(iph->ip_src), inet_ntoa(iph->ip_dst));

  /* Read rest of the packet and call each sort of packet routine. */
  stream_forward_getp(ibuf, EIGRP_HEADER_LEN);


  /* New testing block of code for handling Acks */
  if (ntohl(eigrph->ack) != 0)
    {
      nbr = eigrp_nbr_get(ei, eigrph, iph);

      /* neighbor must be valid, eigrp_nbr_get creates if none existed */
      assert(nbr);

     struct eigrp_packet *ep;

     ep = eigrp_fifo_tail(nbr->retrans_queue);
     if (ep != NULL)
       {
         if (ntohl(eigrph->ack) == ep->sequence_number)
           {
             if((nbr->state == EIGRP_NEIGHBOR_PENDING) && (ntohl(eigrph->ack) == nbr->init_sequence_number))
               {
                 eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_UP);
                 zlog_info("Neighbor adjacency became full");
                 nbr->init_sequence_number = 0;
                 nbr->recv_sequence_number = ntohl(eigrph->sequence);
                 eigrp_update_send_EOT(nbr);
               }
             ep = eigrp_fifo_pop_tail(nbr->retrans_queue);
             /*eigrp_packet_free(ep);*/
             if (nbr->retrans_queue->count > 0)
               {
                 eigrp_send_packet_reliably(nbr);
               }
           }
       }
     ep = eigrp_fifo_tail(nbr->multicast_queue);
     if (ep != NULL)
       {
         if (ntohl(eigrph->ack) == ep->sequence_number)
           {
             ep = eigrp_fifo_pop_tail(nbr->multicast_queue);
             eigrp_packet_free(ep);
             if (nbr->multicast_queue->count > 0)
               {
                 eigrp_send_packet_reliably(nbr);
               }
           }
       }
    }


  switch (opcode)
    {
    case EIGRP_OPC_HELLO:
      eigrp_hello_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_PROBE:
      //      eigrp_probe_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_QUERY:
      eigrp_query_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_REPLY:
      eigrp_reply_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_REQUEST:
      //      eigrp_request_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_SIAQUERY:
      eigrp_query_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_SIAREPLY:
      eigrp_reply_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    case EIGRP_OPC_UPDATE:
      eigrp_update_receive(eigrp, iph, eigrph, ibuf, ei, length);
      break;
    default:
      zlog(NULL, LOG_WARNING,
	    "interface %s: EIGRP packet header type %d unsupported",
	    IF_NAME(ei), opcode);
      break;
    }

  return 0;
}

static struct stream *
eigrp_recv_packet (int fd, struct interface **ifp, struct stream *ibuf)
{
  int ret;
  struct ip *iph;
  u_int16_t ip_len;
  unsigned int ifindex = 0;
  struct iovec iov;
  /* Header and data both require alignment. */
  char buff[CMSG_SPACE(SOPT_SIZE_CMSG_IFINDEX_IPV4())];
  struct msghdr msgh;

  memset(&msgh, 0, sizeof(struct msghdr));
  msgh.msg_iov = &iov;
  msgh.msg_iovlen = 1;
  msgh.msg_control = (caddr_t) buff;
  msgh.msg_controllen = sizeof(buff);

  ret = stream_recvmsg(ibuf, fd, &msgh, 0, (EIGRP_PACKET_MAX_LEN + 1));
  if (ret < 0)
    {
      zlog_warn("stream_recvmsg failed: %s", safe_strerror(errno));
      return NULL;
    }
  if ((unsigned int) ret < sizeof(iph)) /* ret must be > 0 now */
    {
      zlog_warn("eigrp_recv_packet: discarding runt packet of length %d "
          "(ip header size is %u)", ret, (u_int) sizeof(iph));
      return NULL;
    }

  /* Note that there should not be alignment problems with this assignment
   because this is at the beginning of the stream data buffer. */
  iph = (struct ip *) STREAM_DATA(ibuf);
  sockopt_iphdrincl_swab_systoh(iph);

  ip_len = iph->ip_len;

#if !defined (GNU_LINUX) && (OpenBSD < 200311) && (__FreeBSD_version < 1000000)
  /*
   * Kernel network code touches incoming IP header parameters,
   * before protocol specific processing.
   *
   *   1) Convert byteorder to host representation.
   *      --> ip_len, ip_id, ip_off
   *
   *   2) Adjust ip_len to strip IP header size!
   *      --> If user process receives entire IP packet via RAW
   *          socket, it must consider adding IP header size to
   *          the "ip_len" field of "ip" structure.
   *
   * For more details, see <netinet/ip_input.c>.
   */
  ip_len = ip_len + (iph->ip_hl << 2);
#endif

#if defined (__DragonFly__)
  /*
   * in DragonFly's raw socket, ip_len/ip_off are read
   * in network byte order.
   * As OpenBSD < 200311 adjust ip_len to strip IP header size!
   */
  ip_len = ntohs(iph->ip_len) + (iph->ip_hl << 2);
#endif

  ifindex = getsockopt_ifindex(AF_INET, &msgh);

  *ifp = if_lookup_by_index(ifindex);

  if (ret != ip_len)
    {
      zlog_warn("eigrp_recv_packet read length mismatch: ip_len is %d, "
		"but recvmsg returned %d", ip_len, ret);
      return NULL;
    }

  return ibuf;
}

struct eigrp_fifo *
eigrp_fifo_new (void)
{
  struct eigrp_fifo *new;

  new = XCALLOC(MTYPE_EIGRP_FIFO, sizeof(struct eigrp_fifo));
  return new;
}

/* Free eigrp packet fifo. */
void
eigrp_fifo_free (struct eigrp_fifo *fifo)
{
  struct eigrp_packet *ep;
  struct eigrp_packet *next;

  for (ep = fifo->head; ep; ep = next)
    {
      next = ep->next;
      eigrp_packet_free(ep);
    }
  fifo->head = fifo->tail = NULL;
  fifo->count = 0;

  XFREE(MTYPE_EIGRP_FIFO, fifo);
}

/* Free eigrp fifo entries without destroying fifo itself*/
void
eigrp_fifo_reset (struct eigrp_fifo *fifo)
{
  struct eigrp_packet *ep;
  struct eigrp_packet *next;

  for (ep = fifo->head; ep; ep = next)
    {
      next = ep->next;
      eigrp_packet_free(ep);
    }
  fifo->head = fifo->tail = NULL;
  fifo->count = 0;
}

struct eigrp_packet *
eigrp_packet_new (size_t size)
{
  struct eigrp_packet *new;

  new = XCALLOC(MTYPE_EIGRP_PACKET, sizeof(struct eigrp_packet));
  new->s = stream_new(size);
  new->retrans_counter = 0;

  return new;
}

void
eigrp_send_packet_reliably (struct eigrp_neighbor *nbr)
{
  struct eigrp_packet *ep;

  ep = eigrp_fifo_tail(nbr->retrans_queue);

  if (ep)
    {
      struct eigrp_packet *duplicate;
      duplicate = eigrp_packet_duplicate(ep, nbr);
      /* Add packet to the top of the interface output queue*/
      eigrp_fifo_push_head(nbr->ei->obuf, duplicate);

      /*Start retransmission timer*/
      THREAD_TIMER_ON(master, ep->t_retrans_timer, eigrp_unack_packet_retrans,
          nbr, EIGRP_PACKET_RETRANS_TIME);

      /*Increment sequence number counter*/
      nbr->ei->eigrp->sequence_number++;

      /* Hook thread to write packet. */
      if (nbr->ei->on_write_q == 0)
        {
          listnode_add(nbr->ei->eigrp->oi_write_q, nbr->ei);
          nbr->ei->on_write_q = 1;
        }
      if (nbr->ei->eigrp->t_write == NULL)
        nbr->ei->eigrp->t_write =
            thread_add_write(master, eigrp_write, nbr->ei->eigrp, nbr->ei->eigrp->fd);
    }
}

/* Calculate EIGRP checksum */
void
eigrp_packet_checksum (struct eigrp_interface *ei, struct stream *s,
		       u_int16_t length)
{
  struct eigrp_header *eigrph;

  eigrph = (struct eigrp_header *) STREAM_DATA(s);

  /* Calculate checksum. */
  eigrph->checksum = in_cksum(eigrph, length);
}

/* Make EIGRP header. */
void
eigrp_packet_header_init (int type, struct eigrp_interface *ei, struct stream *s,
			  u_int32_t flags, u_int32_t sequence, u_int32_t ack)
{
  struct eigrp_header *eigrph;

  eigrph = (struct eigrp_header *) STREAM_DATA(s);

  eigrph->version = (u_char) EIGRP_HEADER_VERSION;
  eigrph->opcode = (u_char) type;
  eigrph->checksum = 0;

  eigrph->vrid = htons(ei->eigrp->vrid);
  eigrph->ASNumber = htons(ei->eigrp->AS);
  eigrph->ack = htonl(ack);
  eigrph->sequence = htonl(sequence);
//  if(flags == EIGRP_INIT_FLAG)
//    eigrph->sequence = htonl(3);
  eigrph->flags = htonl(flags);

  if (IS_DEBUG_EIGRP_TRANSMIT(0, RECV))
    zlog_debug("Packet Header Init Seq [%u] Ack [%u]",
	       htonl(eigrph->sequence), htonl(eigrph->ack));

  stream_forward_endp(s, EIGRP_HEADER_LEN);
}

/* Add new packet to head of fifo. */
void
eigrp_fifo_push_head (struct eigrp_fifo *fifo, struct eigrp_packet *ep)
{
  ep->next = fifo->head;
  ep->previous = NULL;

  if (fifo->tail == NULL)
    fifo->tail = ep;

  if (fifo->count != 0)
    fifo->head->previous = ep;

  fifo->head = ep;

  fifo->count++;
}

/* Return first fifo entry. */
struct eigrp_packet *
eigrp_fifo_head (struct eigrp_fifo *fifo)
{
  return fifo->head;
}

/* Return last fifo entry. */
struct eigrp_packet *
eigrp_fifo_tail (struct eigrp_fifo *fifo)
{
  return fifo->tail;
}

void
eigrp_packet_delete (struct eigrp_interface *ei)
{
  struct eigrp_packet *ep;

  ep = eigrp_fifo_pop (ei->obuf);

  if (ep)
    eigrp_packet_free(ep);
}

/* Delete first packet from fifo. */
struct eigrp_packet *
eigrp_fifo_pop (struct eigrp_fifo *fifo)
{
  struct eigrp_packet *ep;

  ep = fifo->head;

  if (ep)
    {
      fifo->head = ep->next;

      if (fifo->head == NULL)
        fifo->tail = NULL;
      else
        fifo->head->previous = NULL;

      fifo->count--;
    }

  return ep;
}

void
eigrp_packet_free (struct eigrp_packet *ep)
{
  if (ep->s)
    stream_free(ep->s);

  THREAD_OFF(ep->t_retrans_timer);

  XFREE(MTYPE_EIGRP_PACKET, ep);

  ep = NULL;
}

/* EIGRP Header verification. */
static int
eigrp_verify_header (struct stream *ibuf, struct eigrp_interface *ei,
    struct ip *iph, struct eigrp_header *eigrph)
{

  /* Check network mask, Silently discarded. */
  if (!eigrp_check_network_mask(ei, iph->ip_src))
    {
      zlog_warn("interface %s: eigrp_read network address is not same [%s]",
          IF_NAME(ei), inet_ntoa(iph->ip_src));
      return -1;
    }
//
//  /* Check authentication. The function handles logging actions, where required. */
//  if (! eigrp_check_auth(ei, eigrph))
//    return -1;

  return 0;
}

/* Unbound socket will accept any Raw IP packets if proto is matched.
 To prevent it, compare src IP address and i/f address with masking
 i/f network mask. */
static int
eigrp_check_network_mask (struct eigrp_interface *ei, struct in_addr ip_src)
{
  struct in_addr mask, me, him;

  if (ei->type == EIGRP_IFTYPE_POINTOPOINT)
    return 1;

  masklen2ip(ei->address->prefixlen, &mask);

  me.s_addr = ei->address->u.prefix4.s_addr & mask.s_addr;
  him.s_addr = ip_src.s_addr & mask.s_addr;

  if (IPV4_ADDR_SAME(&me, &him))
    return 1;

  return 0;
}

int
eigrp_unack_packet_retrans (struct thread *thread)
{
  struct eigrp_neighbor *nbr;
  nbr = (struct eigrp_neighbor *) THREAD_ARG(thread);

  struct eigrp_packet *ep;
  ep = eigrp_fifo_tail(nbr->retrans_queue);

  if (ep)
    {
      struct eigrp_packet *duplicate;
      duplicate = eigrp_packet_duplicate(ep, nbr);

      /* Add packet to the top of the interface output queue*/
      eigrp_fifo_push_head(nbr->ei->obuf, duplicate);

      ep->retrans_counter++;
      if(ep->retrans_counter == EIGRP_PACKET_RETRANS_MAX)
        return eigrp_retrans_count_exceeded(ep, nbr);

      /*Start retransmission timer*/
      ep->t_retrans_timer =
          thread_add_timer(master, eigrp_unack_packet_retrans, nbr,EIGRP_PACKET_RETRANS_TIME);

      /* Hook thread to write packet. */
      if (nbr->ei->on_write_q == 0)
        {
          listnode_add(nbr->ei->eigrp->oi_write_q, nbr->ei);
          nbr->ei->on_write_q = 1;
        }
      if (nbr->ei->eigrp->t_write == NULL)
        nbr->ei->eigrp->t_write =
            thread_add_write(master, eigrp_write, nbr->ei->eigrp, nbr->ei->eigrp->fd);
    }

  return 0;
}

int
eigrp_unack_multicast_packet_retrans (struct thread *thread)
{
  struct eigrp_neighbor *nbr;
  nbr = (struct eigrp_neighbor *) THREAD_ARG(thread);

  struct eigrp_packet *ep;
  ep = eigrp_fifo_tail(nbr->multicast_queue);

  if (ep)
    {
      struct eigrp_packet *duplicate;
      duplicate = eigrp_packet_duplicate(ep, nbr);
      /* Add packet to the top of the interface output queue*/
      eigrp_fifo_push_head(nbr->ei->obuf, duplicate);

      ep->retrans_counter++;
      if(ep->retrans_counter == EIGRP_PACKET_RETRANS_MAX)
        return eigrp_retrans_count_exceeded(ep, nbr);

      /*Start retransmission timer*/
      ep->t_retrans_timer =
          thread_add_timer(master, eigrp_unack_multicast_packet_retrans, nbr,EIGRP_PACKET_RETRANS_TIME);

      /* Hook thread to write packet. */
      if (nbr->ei->on_write_q == 0)
        {
          listnode_add(nbr->ei->eigrp->oi_write_q, nbr->ei);
          nbr->ei->on_write_q = 1;
        }
      if (nbr->ei->eigrp->t_write == NULL)
        nbr->ei->eigrp->t_write =
            thread_add_write(master, eigrp_write, nbr->ei->eigrp, nbr->ei->eigrp->fd);
    }

  return 0;
}

/* Get packet from tail of fifo. */
struct eigrp_packet *
eigrp_fifo_pop_tail (struct eigrp_fifo *fifo)
{
  struct eigrp_packet *ep;

  ep = fifo->tail;

  if (ep)
    {
      fifo->tail = ep->previous;

      if (fifo->tail == NULL)
        fifo->head = NULL;
      else
        fifo->tail->next = NULL;

      fifo->count--;
    }

  return ep;
}

struct eigrp_packet *
eigrp_packet_duplicate (struct eigrp_packet *old, struct eigrp_neighbor *nbr)
{
  struct eigrp_packet *new;

  new = eigrp_packet_new(nbr->ei->ifp->mtu);
  new->length = old->length;
  new->retrans_counter = old->retrans_counter;
  new->dst = old->dst;
  new->sequence_number = old->sequence_number;
  stream_copy(new->s, old->s);

  return new;
}

struct TLV_IPv4_Internal_type *
eigrp_read_ipv4_tlv (struct stream *s)
{
  struct TLV_IPv4_Internal_type *tlv;

  tlv = eigrp_IPv4_InternalTLV_new ();

  tlv->type = stream_getw(s);
  tlv->length = stream_getw(s);
  tlv->forward.s_addr = stream_getl(s);
  tlv->metric.delay = stream_getl(s);
  tlv->metric.bandwith = stream_getl(s);
  tlv->metric.mtu[0] = stream_getc(s);
  tlv->metric.mtu[1] = stream_getc(s);
  tlv->metric.mtu[2] = stream_getc(s);
  tlv->metric.hop_count = stream_getc(s);
  tlv->metric.reliability = stream_getc(s);
  tlv->metric.load = stream_getc(s);
  tlv->metric.tag = stream_getc(s);
  tlv->metric.flags = stream_getc(s);

  tlv->prefix_length = stream_getc(s);

  if (tlv->prefix_length <= 8)
    {
      tlv->destination_part[0] = stream_getc(s);
      tlv->destination.s_addr = (tlv->destination_part[0]);

    }
  else if (tlv->prefix_length > 8 && tlv->prefix_length <= 16)
    {
      tlv->destination_part[0] = stream_getc(s);
      tlv->destination_part[1] = stream_getc(s);
      tlv->destination.s_addr = ((tlv->destination_part[1] << 8)
          + tlv->destination_part[0]);
    }
  else if (tlv->prefix_length > 16 && tlv->prefix_length <= 24)
    {
      tlv->destination_part[0] = stream_getc(s);
      tlv->destination_part[1] = stream_getc(s);
      tlv->destination_part[2] = stream_getc(s);
      tlv->destination.s_addr = ((tlv->destination_part[2] << 16)
          + (tlv->destination_part[1] << 8) + tlv->destination_part[0]);
    }
  else if (tlv->prefix_length > 24 && tlv->prefix_length <= 32)
    {
      tlv->destination_part[0] = stream_getc(s);
      tlv->destination_part[1] = stream_getc(s);
      tlv->destination_part[2] = stream_getc(s);
      tlv->destination_part[3] = stream_getc(s);
      tlv->destination.s_addr = ((tlv->destination_part[3] << 24)
          + (tlv->destination_part[2] << 16) + (tlv->destination_part[1] << 8)
          + tlv->destination_part[0]);
    }
  return tlv;
}

u_int16_t
eigrp_add_internalTLV_to_stream (struct stream *s,
    struct eigrp_prefix_entry *pe)
{
  u_int16_t length;

  stream_putw(s, EIGRP_TLV_IPv4_INT);
  if (pe->destination_ipv4->prefixlen <= 8)
    {
      stream_putw(s, 0x001A);
      length = 0x001A;
    }
  if ((pe->destination_ipv4->prefixlen > 8)
      && (pe->destination_ipv4->prefixlen <= 16))
    {
      stream_putw(s, 0x001B);
      length = 0x001B;
    }
  if ((pe->destination_ipv4->prefixlen > 16)
      && (pe->destination_ipv4->prefixlen <= 24))
    {
      stream_putw(s, 0x001C);
      length = 0x001C;
    }
  if (pe->destination_ipv4->prefixlen > 24)
    {
      stream_putw(s, 0x001D);
      length = 0x001D;
    }

  stream_putl(s, 0x00000000);

  /*Metric*/
  stream_putl(s, pe->reported_metric.delay);
  stream_putl(s, pe->reported_metric.bandwith);
  stream_putc(s, pe->reported_metric.mtu[2]);
  stream_putc(s, pe->reported_metric.mtu[1]);
  stream_putc(s, pe->reported_metric.mtu[0]);
  stream_putc(s, pe->reported_metric.hop_count);
  stream_putc(s, pe->reported_metric.reliability);
  stream_putc(s, pe->reported_metric.load);
  stream_putc(s, pe->reported_metric.tag);
  stream_putc(s, pe->reported_metric.flags);

  stream_putc(s, pe->destination_ipv4->prefixlen);

  if (pe->destination_ipv4->prefixlen <= 8)
    {
      stream_putc(s, pe->destination_ipv4->prefix.s_addr & 0xFF);
    }
  if ((pe->destination_ipv4->prefixlen > 8)
      && (pe->destination_ipv4->prefixlen <= 16))
    {
      stream_putc(s, pe->destination_ipv4->prefix.s_addr & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 8) & 0xFF);
    }
  if ((pe->destination_ipv4->prefixlen > 16)
      && (pe->destination_ipv4->prefixlen <= 24))
    {
      stream_putc(s, pe->destination_ipv4->prefix.s_addr & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 8) & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 16) & 0xFF);
    }
  if (pe->destination_ipv4->prefixlen > 24)
    {
      stream_putc(s, pe->destination_ipv4->prefix.s_addr & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 8) & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 16) & 0xFF);
      stream_putc(s, (pe->destination_ipv4->prefix.s_addr >> 24) & 0xFF);
    }

  return length;
}

u_int16_t
eigrp_add_authTLV_MD5_to_stream (struct stream *s,
    struct eigrp_interface *ei)
{
  struct key *key;
  struct keychain *keychain;
  struct TLV_MD5_Authentication_Type *authTLV;

  authTLV = eigrp_authTLV_MD5_new();

  authTLV->type = htons(EIGRP_TLV_AUTH);
  authTLV->length = htons(EIGRP_AUTH_MD5_TLV_SIZE);
  authTLV->auth_type = htons(EIGRP_AUTH_TYPE_MD5);
  authTLV->auth_length = htons(EIGRP_AUTH_TYPE_MD5_LEN);
  authTLV->key_sequence = 0;
  memset(authTLV->Nullpad,0,sizeof(authTLV->Nullpad));


  keychain = keychain_lookup(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
  if(keychain)
    key = key_lookup_for_send(keychain);
  else
    {
      free(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
      IF_DEF_PARAMS (ei->ifp)->auth_keychain = NULL;
      eigrp_authTLV_MD5_free(authTLV);
      return 0;
    }

  if(key)
    {
      authTLV->key_id = htonl(key->index);
      memset(authTLV->digest,0,EIGRP_AUTH_TYPE_MD5_LEN);
      stream_put(s,authTLV, sizeof(struct TLV_MD5_Authentication_Type));
      eigrp_authTLV_MD5_free(authTLV);
      return EIGRP_AUTH_MD5_TLV_SIZE;
    }

  eigrp_authTLV_MD5_free(authTLV);

  return 0;
}

u_int16_t
eigrp_add_authTLV_SHA256_to_stream (struct stream *s,
    struct eigrp_interface *ei)
{
  struct key *key;
  struct keychain *keychain;
  struct TLV_SHA256_Authentication_Type *authTLV;

  authTLV = eigrp_authTLV_SHA256_new();

  authTLV->type = htons(EIGRP_TLV_AUTH);
  authTLV->length = htons(EIGRP_AUTH_SHA256_TLV_SIZE);
  authTLV->auth_type = htons(EIGRP_AUTH_TYPE_SHA256);
  authTLV->auth_length = htons(EIGRP_AUTH_TYPE_SHA256_LEN);
  authTLV->key_sequence = 0;
  memset(authTLV->Nullpad,0,sizeof(authTLV->Nullpad));


  keychain = keychain_lookup(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
  if(keychain)
    key = key_lookup_for_send(keychain);
  else
    {
      free(IF_DEF_PARAMS (ei->ifp)->auth_keychain);
      IF_DEF_PARAMS (ei->ifp)->auth_keychain = NULL;
      eigrp_authTLV_SHA256_free(authTLV);
      return 0;
    }

  if(key)
    {
      authTLV->key_id = 0;
      memset(authTLV->digest,0,EIGRP_AUTH_TYPE_SHA256_LEN);
      stream_put(s,authTLV, sizeof(struct TLV_SHA256_Authentication_Type));
      eigrp_authTLV_SHA256_free(authTLV);
      return EIGRP_AUTH_SHA256_TLV_SIZE;
    }

  eigrp_authTLV_SHA256_free(authTLV);

  return 0;

}

struct TLV_MD5_Authentication_Type *
eigrp_authTLV_MD5_new ()
{
  struct TLV_MD5_Authentication_Type *new;

  new = XCALLOC(MTYPE_EIGRP_AUTH_TLV, sizeof(struct TLV_MD5_Authentication_Type));

  return new;
}

void
eigrp_authTLV_MD5_free (struct TLV_MD5_Authentication_Type *authTLV)
{

  XFREE(MTYPE_EIGRP_AUTH_TLV, authTLV);
}

struct TLV_SHA256_Authentication_Type *
eigrp_authTLV_SHA256_new ()
{
  struct TLV_SHA256_Authentication_Type *new;

  new = XCALLOC(MTYPE_EIGRP_AUTH_SHA256_TLV, sizeof(struct TLV_SHA256_Authentication_Type));

  return new;
}

void
eigrp_authTLV_SHA256_free (struct TLV_SHA256_Authentication_Type *authTLV)
{

  XFREE(MTYPE_EIGRP_AUTH_SHA256_TLV, authTLV);
}


struct TLV_IPv4_Internal_type *
eigrp_IPv4_InternalTLV_new ()
{
  struct TLV_IPv4_Internal_type *new;

  new = XCALLOC(MTYPE_EIGRP_IPV4_INT_TLV,sizeof(struct TLV_IPv4_Internal_type));

  return new;
}

void
eigrp_IPv4_InternalTLV_free (struct TLV_IPv4_Internal_type *IPv4_InternalTLV)
{

  XFREE(MTYPE_EIGRP_IPV4_INT_TLV, IPv4_InternalTLV);
}

struct TLV_Sequence_Type *
eigrp_SequenceTLV_new ()
{
  struct TLV_Sequence_Type *new;

  new = XCALLOC(MTYPE_EIGRP_SEQ_TLV,sizeof(struct TLV_Sequence_Type));

  return new;
}
