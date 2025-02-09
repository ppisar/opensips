/*
 * Copyright (C) 2022 - OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "../../ut.h"
#include "../../forward.h"
#include "../../md5.h"
#include "../../timer.h"
#include "../../rw_locking.h"
#include "../../mem/mem.h"
#include "../../lib/hash.h"
#include "msrp_parser.h"
#include "msrp_signaling.h"

#define MSRP_PREFIX "MSRP "
#define MSRP_PREFIX_LEN (sizeof(MSRP_PREFIX) - 1)

#define EOM_PREFIX "-------"
#define EOM_PREFIX_LEN (sizeof(EOM_PREFIX) - 1)

#define TO_PATH_PREFIX "To-Path: "
#define TO_PATH_PREFIX_LEN (sizeof(TO_PATH_PREFIX) - 1)

#define FROM_PATH_PREFIX "From-Path: "
#define FROM_PATH_PREFIX_LEN (sizeof(FROM_PATH_PREFIX) - 1)

/* convenience macro */
#define  append_string(_d,_s,_len) \
	do{\
		memcpy((_d),(_s),(_len));\
		(_d) += (_len);\
	}while(0);


unsigned int msrp_ident_hash_size = 256;
unsigned int msrp_ident_timeout = 30;

static unsigned short table_curr_idx = 0;
static gen_hash_t **msrp_table = NULL;
static rw_lock_t *ident_lock = NULL;
static handle_trans_timeout_f *handle_trans_timeout = NULL;


static char * _ident_builder( unsigned short hash, unsigned short idx,
		char *padding, int padding_len,
		int *ident_len);

static struct msrp_cell* _build_transaction(struct msrp_msg *req, int hash,
		str *ident);

static void msrp_timer(unsigned int ticks, void* param);

#define MSRP_DEBUG


int msrp_send_reply( void *hdl, struct msrp_msg *req, int code, str* reason,
		str *hdrs, int hdrs_no)
{
	char *buf, *p;
	str to_body, from_body;
	int i, len = 0;

	if (code<100 || code>999) {
		LM_ERR("invalid status reply %d, must be [100..999]\n",code);
		return -1;
	}

	/* compute the lenght of the reply*/

	/* first line
	 * MSRP SP transact-id SP status-code [SP comment] CRLF
	 */
	len += MSRP_PREFIX_LEN + req->fl.ident.len + 1 + 3
		+ (reason?(1 + reason->len):0) + CRLF_LEN;

	/* headers
	 * headers = To-Path CRLF From-Path CRLF 1*( header CRLF )
	 */
	if (req->fl.u.request.method_id==MSRP_METHOD_SEND) {
		/* we need to parse the From-Path too, to get the first URL only */
		if (req->from_path->parsed == NULL) {
			req->from_path->parsed = parse_msrp_path( &req->from_path->body);
			if (req->from_path->parsed == NULL) {
				LM_ERR("Invalid From-Path payload :(\n");
				return -1;
			}
		}
		to_body = ((struct msrp_url*)(req->from_path->parsed))->whole;
	} else {
		/* take the whole list of URLs from the From-Path*/
		to_body = req->from_path->body;
	}
	/* as FROM use the first URL from TO, which is already parsed */
	from_body = ((struct msrp_url*)(req->to_path->parsed))->whole;
	/* and now let's calcalate */
	len += TO_PATH_PREFIX_LEN + to_body.len + CRLF_LEN
		+ FROM_PATH_PREFIX_LEN + from_body.len + CRLF_LEN;
	/* add the hdrs */
	for ( i=0 ; i<hdrs_no ; i++)
		len += hdrs[i].len + CRLF_LEN;

	 /* EOM
	  * end-line = "-------" transact-id continuation-flag CRLF
	  */
	len += EOM_PREFIX_LEN + req->fl.ident.len + 1 + CRLF_LEN;


	/* allocate the buffer */
	buf = pkg_malloc( len );
	if (buf==NULL) {
		LM_ERR("failed to pkg allocate the reply buffer\n");
		return -1;
	}


	/* start building */
	p = buf;

	/* first line */
	append_string( p, MSRP_PREFIX, MSRP_PREFIX_LEN);
	append_string( p, req->fl.ident.s, req->fl.ident.len);
	*(p++) = ' ';
	p += btostr( p, code );
	if (reason) {
		*(p++) = ' ';
		append_string( p, reason->s, reason->len);
	}
	append_string( p, CRLF, CRLF_LEN);

	/* headers */
	append_string( p, TO_PATH_PREFIX, TO_PATH_PREFIX_LEN);
	append_string( p, to_body.s, to_body.len);
	append_string( p, CRLF, CRLF_LEN);

	append_string( p, FROM_PATH_PREFIX, FROM_PATH_PREFIX_LEN);
	append_string( p, from_body.s, from_body.len);
	append_string( p, CRLF, CRLF_LEN);

	for ( i=0 ; i<hdrs_no ; i++) {
		append_string( p,  hdrs[i].s,  hdrs[i].len);
		append_string( p, CRLF, CRLF_LEN);
	}

	/* EOM */
	append_string( p, EOM_PREFIX, EOM_PREFIX_LEN);
	append_string( p, req->fl.ident.s, req->fl.ident.len);
	*(p++) = '$';
	append_string( p, CRLF, CRLF_LEN);

	if (p-buf!=len) {
		LM_BUG("computed %d, but wrote %d :(\n",len,(int)(p-buf));
		goto error;
	}

	/* now, send it out*/
	i = msg_send( req->rcv.bind_address, PROTO_MSRP,
			&req->rcv.src_su, req->rcv.proto_reserved1,
			buf, len, NULL);
	if (i<0) {
		/* sending failed, FIXME - close the connection */
		LM_ERR("failed to send MSRP reply\n");
		goto error;
	}

	pkg_free(buf);
	return 0;

error:
	pkg_free(buf);
	return -1;
}


/* Returns :
 *  -1 - bad request
 *  -2 - cannot resolve destination
 *  -3 - internal error
 */
int msrp_fwd_request( void *hdl, struct msrp_msg *req, str *hdrs, int hdrs_no)
{
	char *buf, *p, *s, bk;
	struct msrp_url *to, *from;
	union sockaddr_union su;
	struct hostent* he;
	int i, len, hash, idx;
	char md5[MD5_LEN];
	str ident, md5_src[3];
	struct msrp_cell *cell;
	void **val;

	if (req==NULL)
		return -1;

	/* we need both TO and FROM path hdrs to be parsed. The TO should be
	 * already, so let's do the FROM */
	if (req->from_path->parsed == NULL) {
		req->from_path->parsed = parse_msrp_path( &req->from_path->body);
		if (req->from_path->parsed == NULL) {
			LM_ERR("Invalid From-Path payload :(\n");
			return -1;
		}
	}
	from = ((struct msrp_url*)(req->from_path->parsed));
	to   = ((struct msrp_url*)(req->to_path->parsed));

	/* we need to move the top path from TO to FROM, while keeping the
	 * the whole message the same */

	if (to->next==NULL) {
		LM_ERR("cannot forward as there is no second URL in TO-PATH\n");
		return -1;
	}

	/* before doing and heavy lifting (as building the out buffer), let's
	 * resolve the destination first. */
	bk = to->next->host.s[to->next->host.len]; // usual hack
	to->next->host.s[to->next->host.len] = 0;
	he = resolvehost( to->next->host.s, 0/*no_ip_test*/); // FIXME - do SRV
	to->next->host.s[to->next->host.len] = bk;
	if (he==NULL) {
		LM_ERR("Could not resolve the destination <%.*s>\n",
			to->next->host.len, to->next->host.s);
		return -2;
	}
	if ( to->next->port_no==0 ) {
		LM_BUG("Add the check or SRV support !!\n");
		return -2;
	}
	if ( hostent2su( &su, he, 0/*idx*/, to->next->port_no )!=0 ) {
		LM_ERR("Could translate he to su :-/, bad familly type??\n");
		return -2;
	}


	/* decide which hash to use for the transaction */
	lock_start_read( ident_lock );
	idx = table_curr_idx;
	lock_stop_read( ident_lock );


redo_ident:
	/* compute the new ident first */
	hash = hash_entry( msrp_table[idx] , req->fl.ident);
#ifdef MSRP_DEBUG
	LM_DBG("using idx %d, hash %d  over [%.*s] (size is %d)\n",
		idx, hash, req->fl.ident.len, req->fl.ident.s, msrp_table[idx]->size);
#endif
	i = 0;
	md5_src[i++] = to->whole;
	md5_src[i++] = from->whole;
	if (req->message_id)
		md5_src[i++] = req->message_id->body;
	MD5StringArray( md5, md5_src, i);
	ident.s  = _ident_builder( hash, idx, md5, MD5_LEN, &ident.len);


	/* the len will be the same after moving the URL, the only diff will
	 * be imposed by any extra hdrs and diff in ident len (twice!) */
	len = req->len + 2 * (ident.len - req->fl.ident.len);
	if (hdrs_no>0 && hdrs) 
		for( i=0 ; i<hdrs_no ; i++ )
			len += hdrs[i].len + CRLF_LEN;


	/* allocate the buffer */
	buf = pkg_malloc( len );
	if (buf==NULL) {
		LM_ERR("failed to pkg allocate the request buffer\n");
		return -3;
	}


	/* start building */
	p = buf;
	s = req->buf;

	/* copy everything up to the ident, which needs to be replaced here */
	append_string( p, s, (int)(req->fl.ident.s-s));
	/* put our new ident */
	append_string( p, ident.s, ident.len);
	/* TO is the first hdr, so copy everything up to its first URL (which
	 * needs to be skipped here) */
	s = req->fl.ident.s + req->fl.ident.len;
	append_string( p, s, (int)(to->whole.s-s));
	/* copy starting with the second URL, all the way to the first FROM URL */
	s = to->next->whole.s;
	append_string( p, s, (int)(from->whole.s-s));
	/* first place here the first TO URL that was skipped */
	append_string( p, to->whole.s, to->whole.len);
	*(p++) = ' ';
	/* copy starting with the first FROM URL */
	s = from->whole.s;
	if (hdrs_no>0 && hdrs) {
		/* copy up to the end of the last hdr (including its CRLF) */
		append_string( p, s,
			(int)(req->last_header->name.s+req->last_header->len -s));
		/* add the new extra hdrs */
		for ( i=0 ; i<hdrs_no ; i++) {
			append_string( p,  hdrs[i].s,  hdrs[i].len);
			append_string( p, CRLF, CRLF_LEN);
		}
		/* copy from the end of the last hdr all the way to the end of buffer*/
		s = req->last_header->name.s + req->last_header->len;
	}/* else {
		nothing to append, copy all the way to the end of buffer
	}*/
	append_string( p, s,
		(int)(req->buf+req->len-s-req->fl.ident.len-CRLF_LEN-1));
	/* put our new ident */
	append_string( p, ident.s, ident.len);
	s = req->buf+req->len-CRLF_LEN-1;
	append_string( p, s, CRLF_LEN+1 );

	if (p-buf!=len) {
		LM_BUG("computed %d, but wrote %d :(\n",len,(int)(p-buf));
		goto error;
	}
#ifdef MSRP_DEBUG
	LM_DBG("----|\n%.*s|-----\n",len,buf);
#endif


	/* do transactional stuff */
	cell = _build_transaction( req, hash, &ident);
	if (cell==NULL) {
		LM_ERR("failed to build transaction, not sending request out\n");
		goto error;
	}
	/* remember the handler that created this transaction */
	cell->msrp_hdl = hdl;
	/* add trasaction to hash table.... */
	hash_lock( msrp_table[idx], hash);

	val = hash_get(  msrp_table[idx], hash, ident);
	if (val==NULL) {
		hash_unlock( msrp_table[idx], hash);
		msrp_free_transaction( cell );
		LM_ERR("failed to insert transaction into hash, "
			"dropping everything\n");
		goto error;
	} else
	if (*val!=NULL) {
		/* duplicate :O, try generating another ident */
		hash_unlock( msrp_table[idx], hash);
		pkg_free(buf);
		goto redo_ident;
	}
	*val = cell;

	hash_unlock( msrp_table[idx], hash);


	/* now, send it out*/
	// TODO - for now we use the same socket (as the received one), but
	//        it will nice to be able to change it (via script??) in order
	//        to do traffic bridging between 2 interfaces.
	i = msg_send( req->rcv.bind_address, PROTO_MSRP, &su, 0 /*conn-id*/,
			buf, len, NULL);
	if (i<0) {
		/* sending failed, TODO - close the connection */
		LM_ERR("failed to fwd MSRP request\n");
		/* trash the current transaction */
		hash_lock( msrp_table[idx], hash);
		hash_remove(  msrp_table[idx], hash, ident);
		hash_unlock( msrp_table[idx], hash);
		msrp_free_transaction( cell );
		goto error;
	}

	pkg_free(buf);
	return 0;

error:
	pkg_free(buf);
	return -3;
}


int msrp_fwd_reply( void *hdl, struct msrp_msg *rpl, struct msrp_cell *cell)
{
	char *buf, *p, *s;
	struct msrp_url *to, *from;
	int i, len;

	if (rpl==NULL || cell==NULL)
		return -1;

	/* we need both TO and FROM path hdrs to be parsed. The TO should be
	 * already, so let's do the FROM */
	if (rpl->from_path->parsed == NULL) {
		rpl->from_path->parsed = parse_msrp_path( &rpl->from_path->body);
		if (rpl->from_path->parsed == NULL) {
			LM_ERR("Invalid From-Path payload :(\n");
			return -1;
		}
	}
	from = ((struct msrp_url*)(rpl->from_path->parsed));
	to   = ((struct msrp_url*)(rpl->to_path->parsed));

	/* we need to move the top path from TO to FROM, while keeping the
	 * the whole message the same */

	if (to->next==NULL) {
		LM_ERR("cannot forward as there is no second URL in TO-PATH\n");
		return -1;
	}

	/* the len will be the same after moving the URL, the only diff will
	 * be imposed by the diff in ident len (twice!) */
	len = rpl->len + 2 * (cell->recv_ident.len - rpl->fl.ident.len);


	/* allocate the buffer */
	buf = pkg_malloc( len );
	if (buf==NULL) {
		LM_ERR("failed to pkg allocate the request buffer\n");
		return -3;
	}


	/* start building */
	p = buf;
	s = rpl->buf;

	/* copy everything up to the ident, which needs to be replaced here */
	append_string( p, s, (int)(rpl->fl.ident.s-s));
	/* put back the ident received in the request */
	append_string( p, cell->recv_ident.s, cell->recv_ident.len);
	/* TO is the first hdr, so copy everything up to its first URL (which
	 * needs to be skipped here) */
	s = rpl->fl.ident.s + rpl->fl.ident.len;
	append_string( p, s, (int)(to->whole.s-s));
	/* copy starting with the second URL, all the way to the first FROM URL */
	s = to->next->whole.s;
	append_string( p, s, (int)(from->whole.s-s));
	/* first place here the first TO URL that was skipped */
	append_string( p, to->whole.s, to->whole.len);
	*(p++) = ' ';
	/* copy starting with the first FROM URL */
	s = from->whole.s;
	append_string( p, s,
		(int)(rpl->buf+rpl->len-s-rpl->fl.ident.len-CRLF_LEN-1));
	/* put back the received ident */
	append_string( p, cell->recv_ident.s, cell->recv_ident.len);
	s = rpl->buf+rpl->len-CRLF_LEN-1;
	append_string( p, s, CRLF_LEN+1 );

	if (p-buf!=len) {
		LM_BUG("computed %d, but wrote %d :(\n",len,(int)(p-buf));
		goto error;
	}
#ifdef MSRP_DEBUG
	LM_DBG("----|\n%.*s|-----\n",len,buf);
#endif


	/* now, send it out, back to the same spot where the request came from */
	i = msg_send( cell->recv.send_sock, PROTO_MSRP, &cell->recv.to,
			cell->recv.proto_reserved1 /*conn-id*/,
			buf, len, NULL);
	if (i<0) {
		/* sending failed, TODO - close the connection */
		LM_ERR("failed to fwd MSRP request\n");
		goto error;
	}


	pkg_free(buf);
	return 0;

error:
	pkg_free(buf);
	return -3;
}


/********* transactional layer ************/
#define IDENT_SEPARATOR '.'


int msrp_init_trans_layer(handle_trans_timeout_f *timout_f)
{
	int i;

	/* limit the timeout to 30 secs */
	if (msrp_ident_hash_size>30) {
		LM_WARN("ident timeout too big (%d), limiting to 30\n",
			msrp_ident_timeout);
		msrp_ident_timeout = 30;
	}
	/* we want to keep the hash values below 2^10 (there is a hard
	 * limit of 2^16 due the "short" storge and 4 hexa */
	if (msrp_ident_hash_size>1024) {
		LM_WARN("ident hash table too big (%d), limiting to 10\n",
			msrp_ident_hash_size);
		msrp_ident_hash_size = 1024;
	}

	/* build the array oh hashes, one for each second */
	msrp_table = shm_malloc( msrp_ident_timeout * sizeof(gen_hash_t*));
	if (msrp_table==NULL) {
		LM_ERR("failed to init array of ident hashes (size=%d)\n",
			msrp_ident_timeout);
		return -1;
	}
	for ( i=0 ; i<msrp_ident_timeout ; i++ ) {
		msrp_table[i] = hash_init( msrp_ident_hash_size );
		if (msrp_table[i]==NULL) {
			LM_ERR("failed to init ident hash table %d (size=%d)\n",
				i, msrp_ident_hash_size);
			return -1;
		}
	}

	table_curr_idx = 0;

	ident_lock = lock_init_rw();
	if (ident_lock==NULL) {
		LM_ERR("failed to create RW lock for indet table\n");
		return -1;
	}

	if (register_timer( "MSRP timeout", msrp_timer, NULL, 1,
	TIMER_FLAG_DELAY_ON_DELAY) < 0) {
		LM_ERR("failed to register timer\n");
		return -1;
	}

	handle_trans_timeout = timout_f;

	return 0;
}


int msrp_destroy_trans_layer(void)
{
	int i;

	if (msrp_table) {
		for ( i=1 ; i<msrp_ident_timeout ; i++ )
			hash_destroy( msrp_table[i], NULL /*FIXME*/);
		shm_free(msrp_table);
	}

	if (ident_lock)
		lock_destroy_rw( ident_lock );

	return 0;
}


static char * _ident_builder( unsigned short hash, unsigned short idx,
		char *padding, int padding_len,
		int *ident_len)
{
	#define IDENT_BUF_MAX_LEN 20
	/* format is hash_hexa.idx_hexa.rand_hexa.padding , max 20 chars*/
	static char ident_s[IDENT_BUF_MAX_LEN + 1];
	unsigned short rnd;
	int size;
	char *p;

	/* hash+idx+rand hexas are minimum 6 chars overall, leaving a 14 max
	 * for padding. On the other side, with (4+1) max hash+idx+rand hexas
	 * => max 15 chars, leaving another 5 for padding
	 */

	p = ident_s;
	size = IDENT_BUF_MAX_LEN;

	/* start with the hash hexa; it is "short", so it fits on 4 hexa chars */
	if (int2reverse_hex( &p, &size, hash)==-1)
		return NULL;

	*(p++)=IDENT_SEPARATOR;
	size--;

	/* now the idx, which is "short" (usually less than 255) */
	if (int2reverse_hex( &p, &size, idx)==-1)
		return NULL;

	*(p++)=IDENT_SEPARATOR;
	size--;

	/* now put the rand hexa, also up to 65K, to fit on 4 hexa chars */
	rnd = ((1<<16)*((float)rand()/(float)RAND_MAX));
	if (int2reverse_hex( &p, &size, rnd)==-1)
		return NULL;

	*(p++)=IDENT_SEPARATOR;
	size--;

	/* padding */
	if (size>padding_len) {
		memcpy( p, padding, padding_len);
		p += padding_len;
	} else {
		memcpy( p, padding, size);
		p += size;
	}

	*p = 0;

	*ident_len = (int)(p-ident_s);

#ifdef MSRP_DEBUG
	LM_DBG(" new ident is <%.*s>/%d\n",*ident_len,ident_s,*ident_len);
#endif

	return ident_s;
}


static struct msrp_cell* _build_transaction(struct msrp_msg *req, int hash,
		str *ident)
{
	struct msrp_cell *cell;
	struct msrp_url *to;
	char *p;

	to = ((struct msrp_url*)(req->to_path->parsed));

	cell = shm_malloc( sizeof(struct msrp_cell)
			 + ident->len
			 + req->fl.ident.len
			 + req->from_path->body.len
			 + to->whole.len
			 + (req->message_id?req->message_id->body.len:0)
			 + (req->byte_range?req->byte_range->body.len:0)
			 + (req->failure_report?req->failure_report->body.len:0) 
			);
	if (cell==NULL) {
		LM_ERR("failed to sh malloc new transaction\n");
		return NULL;
	}

	memset( cell, 0, sizeof(struct msrp_cell));
	cell->hash = hash;
	p = (char*)(cell+1);

	cell->ident.s = p;
	cell->ident.len = ident->len;
	append_string( p, ident->s, ident->len);

	cell->recv_ident.s = p;
	cell->recv_ident.len = req->fl.ident.len;
	append_string( p, req->fl.ident.s, req->fl.ident.len);

	cell->from_full.s = p;
	cell->from_full.len = req->from_path->body.len;
	append_string( p, req->from_path->body.s, req->from_path->body.len);

	cell->to_top.s = p;
	cell->to_top.len = to->whole.len;
	append_string( p, to->whole.s, to->whole.len );

	if (req->message_id) {
		cell->message_id.s = p;
		cell->message_id.len = req->message_id->body.len;
		append_string( p, req->message_id->body.s, req->message_id->body.len);
	}

	if (req->byte_range) {
		cell->byte_range.s = p;
		cell->byte_range.len = req->byte_range->body.len;
		append_string( p, req->byte_range->body.s, req->byte_range->body.len);
	}

	if (req->failure_report) {
		cell->failure_report.s = p;
		cell->failure_report.len = req->failure_report->body.len;
		append_string( p, req->failure_report->body.s,
			req->failure_report->body.len);
	}

	init_su( &cell->recv.to, &req->rcv.src_ip, req->rcv.src_port);
	cell->recv.proto = req->rcv.proto;
	cell->recv.proto_reserved1 = req->rcv.proto_reserved1;
	cell->recv.send_sock = req->rcv.bind_address;

	return cell;
}


void msrp_free_transaction(struct msrp_cell *cell)
{
	shm_free(cell);
}


/* it is safe to use a global list for collecting the records from the map
 * as this timer function is ran without overlapping (DELAY on DELAY) */
static struct msrp_cell *expired_list;

static void _table_process_each(void * value)
{
	struct msrp_cell *cell = (struct msrp_cell*)value;

	cell->expired_next = expired_list;
	expired_list = cell;
}


static void msrp_timer(unsigned int ticks, void* param)
{
	int i, n;

	/* every second here, incrementing the time index in the table */

	/* move all transactions from the "expired" slot into a simple list,
	 * so we can "consume" separately, without any locking or conflicts */

	lock_start_write( ident_lock );

	expired_list = NULL;

	i = (table_curr_idx + 1) % msrp_ident_timeout ;

	for ( n=0 ; n < msrp_table[i]->size ; n++) {

		hash_lock( msrp_table[i], n);

		/* destroy the whole map and replace it with an empty one */
		map_destroy( msrp_table[i]->entries[n], _table_process_each);
		msrp_table[i]->entries[n] = map_create(AVLMAP_SHARED);
		if ( msrp_table[i]->entries[n] == NULL) {
			LM_ERR("failed to re-create new AVL");
			//FIXME - what should we do here????
		}

		hash_unlock( msrp_table[i], n);
	}

	table_curr_idx = i;

	lock_stop_write( ident_lock );

	/* now handle the expired list one by one */
	handle_trans_timeout( expired_list );
}


static int _ident_parser( str *ident,
		unsigned short *hash, unsigned short *idx)
{
	char *p, *end;
	str hexa;
	unsigned int hval;

	/* split the ident into hash.idx.xxxxxx */
	p = ident->s;
	end = ident->s + ident->len;

	for ( hexa.s=p ; (p<end) && (*p!=IDENT_SEPARATOR) ; p++);
	if ( *p!=IDENT_SEPARATOR)
		goto parse_error;
	hexa.len = p - hexa.s;
	if (reverse_hex2int( hexa.s, hexa.len,  &hval)<0 ||
	hval >= msrp_ident_hash_size)
		goto parse_error;
	*hash = hval;

	p++; /* get over the separator */

	for ( hexa.s=p ; (p<end) && (*p!=IDENT_SEPARATOR) ; p++);
	if ( *p!=IDENT_SEPARATOR)
		goto parse_error;
	hexa.len = p - hexa.s;
	if (reverse_hex2int( hexa.s, hexa.len,  &hval)<0 ||
	hval >= msrp_ident_timeout)
		goto parse_error;
	*idx = hval;

	/* we do not care of the rest of the ident, it will be checked 
	 * only when doing the key searching in the map */
	return 0;
parse_error:
	LM_ERR("failed in [%.*s] at pos %d[%c]\n", ident->len, ident->s,
		(int)(p-ident->s), *p);
	return -1;
}


/* Searches (ident based), removes and returns a transaction from the hash
 * NULL returned if not found
 */
struct msrp_cell *msrp_get_transaction(str *ident)
{
	unsigned short hash, idx;
	struct msrp_cell *cell;

	if (ident->s==NULL || ident->len==0 ||
	_ident_parser( ident, &hash, &idx)<0)
		return NULL;

	LM_DBG("looking for transaction ident [%.*s] on hash %d, idx=%d\n",
		ident->len, ident->s, hash, idx);

	hash_lock( msrp_table[idx], hash);
	cell = hash_remove( msrp_table[idx], hash, *ident);
	hash_unlock( msrp_table[idx], hash);

	if (cell == NULL) {
		LM_DBG("no transaction found with ident [%.*s] on hash %d, idx=%d\n",
			ident->len, ident->s, hash, idx);
		return NULL;
	}

	return cell;
}
