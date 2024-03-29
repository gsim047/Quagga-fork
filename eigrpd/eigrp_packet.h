/*
 * EIGRP General Sending and Receiving of EIGRP Packets.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
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

#ifndef _ZEBRA_EIGRP_PACKET_H
#define _ZEBRA_EIGRP_PACKET_H

struct thread;
struct vty;
struct eigrp_packet;
struct eigrp_neighbor;
struct eigrp_interface;
struct stream;
struct eigrp_fifo;
struct eigrp_prefix_entry;
struct eigrp;
struct ip;
struct eigrp_header;


/*Prototypes*/
extern int eigrp_read (struct thread *);
extern int eigrp_write (struct thread *);

extern struct eigrp_packet *eigrp_packet_new (size_t);
extern struct eigrp_packet *eigrp_packet_duplicate (struct eigrp_packet *, struct eigrp_neighbor *);
extern void eigrp_packet_free (struct eigrp_packet *);
extern void eigrp_packet_delete (struct eigrp_interface *);
extern void eigrp_packet_header_init (int, struct eigrp_interface *, struct stream *,
				     u_int32_t, u_int32_t, u_int32_t);
extern void eigrp_packet_checksum (struct eigrp_interface *, struct stream *, u_int16_t);

extern struct eigrp_fifo *eigrp_fifo_new (void);
extern struct eigrp_packet *eigrp_fifo_head (struct eigrp_fifo *);
extern struct eigrp_packet *eigrp_fifo_tail (struct eigrp_fifo *);
extern struct eigrp_packet *eigrp_fifo_pop (struct eigrp_fifo *);
extern struct eigrp_packet *eigrp_fifo_pop_tail (struct eigrp_fifo *);
extern void eigrp_fifo_push_head (struct eigrp_fifo *, struct eigrp_packet *);
extern void eigrp_fifo_free (struct eigrp_fifo *);
extern void eigrp_fifo_reset (struct eigrp_fifo *);

extern void eigrp_send_packet_reliably (struct eigrp_neighbor *);

extern struct TLV_IPv4_Internal_type *eigrp_read_ipv4_tlv (struct stream *);
extern u_int16_t eigrp_add_internalTLV_to_stream (struct stream *, struct eigrp_prefix_entry *);
extern u_int16_t eigrp_add_authTLV_MD5_to_stream (struct stream *, struct eigrp_interface *);
extern u_int16_t eigrp_add_authTLV_SHA256_to_stream (struct stream *, struct eigrp_interface *);

extern int eigrp_unack_packet_retrans (struct thread *);
extern int eigrp_unack_multicast_packet_retrans (struct thread *);

/*
 * untill there is reason to have their own header, these externs are found in
 * eigrp_hello.c
 */
extern void eigrp_hello_send (struct eigrp_interface *, u_char, struct in_addr *);
extern void eigrp_hello_send_ack (struct eigrp_neighbor *);
extern void eigrp_hello_receive (struct eigrp *, struct ip *, struct eigrp_header *,
				struct stream *, struct eigrp_interface *, int);
extern int  eigrp_hello_timer (struct thread *);

/*
 * These externs are found in eigrp_update.c
 */
extern void eigrp_update_send (struct eigrp_interface *);
extern void eigrp_update_receive (struct eigrp *, struct ip *, struct eigrp_header *,
                                struct stream *, struct eigrp_interface *, int);
extern void eigrp_update_send_all (struct eigrp *, struct eigrp_interface *);
extern void eigrp_update_send_init (struct eigrp_neighbor *);
extern void eigrp_update_send_EOT (struct eigrp_neighbor *);
extern int eigrp_update_send_GR_thread(struct thread *);
extern void eigrp_update_send_GR (struct eigrp_neighbor *, enum GR_type, struct vty *);
extern void eigrp_update_send_interface_GR (struct eigrp_interface *, enum GR_type, struct vty *);
extern void eigrp_update_send_process_GR (struct eigrp *, enum GR_type, struct vty *);

/*
 * These externs are found in eigrp_query.c
 */

extern void eigrp_send_query (struct eigrp_interface *);
extern void eigrp_query_receive (struct eigrp *, struct ip *, struct eigrp_header *,
                                 struct stream *, struct eigrp_interface *, int);
extern u_int32_t eigrp_query_send_all (struct eigrp *);

/*
 * These externs are found in eigrp_reply.c
 */
extern void eigrp_send_reply (struct eigrp_neighbor *, struct eigrp_prefix_entry *);
extern void eigrp_reply_receive (struct eigrp *, struct ip *, struct eigrp_header *,
                                 struct stream *, struct eigrp_interface *, int);

/*
 * These externs are found in eigrp_siaquery.c
 */
extern void eigrp_send_siaquery (struct eigrp_neighbor *, struct eigrp_prefix_entry *);
extern void eigrp_siaquery_receive (struct eigrp *, struct ip *, struct eigrp_header *,
                     struct stream *, struct eigrp_interface *, int);

/*
 * These externs are found in eigrp_siareply.c
 */
extern void eigrp_send_siareply (struct eigrp_neighbor *, struct eigrp_prefix_entry *);
extern void eigrp_siareply_receive (struct eigrp *, struct ip *, struct eigrp_header *,
                     struct stream *, struct eigrp_interface *, int);

extern struct TLV_MD5_Authentication_Type *eigrp_authTLV_MD5_new (void);
extern void eigrp_authTLV_MD5_free (struct TLV_MD5_Authentication_Type *);
extern struct TLV_SHA256_Authentication_Type *eigrp_authTLV_SHA256_new (void);
extern void eigrp_authTLV_SHA256_free (struct TLV_SHA256_Authentication_Type *);

extern int eigrp_make_md5_digest (struct eigrp_interface *, struct stream *,
                                  u_char);
extern int eigrp_check_md5_digest (struct stream *, struct TLV_MD5_Authentication_Type *,
                            struct eigrp_neighbor *, u_char);
extern int eigrp_make_sha256_digest (struct eigrp_interface *, struct stream *, u_char);
extern int eigrp_check_sha256_digest (struct stream *, struct TLV_SHA256_Authentication_Type *,
                            struct eigrp_neighbor *, u_char );


extern struct TLV_IPv4_Internal_type *eigrp_IPv4_InternalTLV_new (void);
extern void eigrp_IPv4_InternalTLV_free (struct TLV_IPv4_Internal_type *);

extern struct TLV_Sequence_Type *eigrp_SequenceTLV_new (void);

extern const struct message eigrp_packet_type_str[];
extern const size_t eigrp_packet_type_str_max;

#endif /* _ZEBRA_EIGRP_PACKET_H */
