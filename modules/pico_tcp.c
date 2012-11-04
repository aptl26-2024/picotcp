#include "pico_tcp.h"
#include "pico_config.h"
#include "pico_eth.h"
#include "pico_socket.h"
#include "pico_stack.h"
#include "pico_socket.h"
#include "pico_queue.h"
#define TCP_SOCK(s) ((struct pico_socket_tcp *)s)
#define SEQN(f) (f?(long_be(((struct pico_tcp_hdr *)(f->transport_hdr))->seq)):0)
#define ACKN(f) (f?(long_be(((struct pico_tcp_hdr *)(f->transport_hdr))->ack)):0)

#define PICO_TCP_RTO_MIN 1000
#define PICO_TCP_RTO_MAX 120000
#define PICO_TCP_IW 2


#define PICO_TCP_LOOKAHEAD      0x00
#define PICO_TCP_FIRST_DUPACK   0x01
#define PICO_TCP_SECOND_DUPACK  0x02
#define PICO_TCP_RECOVER        0x03
#define PICO_TCP_BLACKOUT       0x04
#define PICO_TCP_UNREACHABLE    0x05


RB_HEAD(pico_segment_pool, pico_frame);
RB_PROTOTYPE_STATIC(pico_segment_pool, pico_frame, node, segment_compare);

static inline int seq_compare(uint32_t a, uint32_t b)
{
  uint32_t thresh = ((uint32_t)(-1))>>1;
  if (((a > thresh) && (b > thresh)) || ((a <= thresh) && (b <= thresh))) {
    if (a > b)
      return 1;
    if (b > a)
      return -1;
  } else {
    if (a > b)
      return -2;
    if (b > a)
      return 2;
  }
  return 0;
}

static int segment_compare(struct pico_frame *a, struct pico_frame *b)
{
  return seq_compare(SEQN(a), SEQN(b));
}

RB_GENERATE_STATIC(pico_segment_pool, pico_frame, node, segment_compare);

struct pico_tcp_queue
{
  struct pico_segment_pool pool;
  uint32_t max_size;
  uint32_t size;
  uint32_t frames;
};


static struct pico_frame *peek_segment(struct pico_tcp_queue *tq, uint32_t seq)
{
  struct pico_tcp_hdr H;
  struct pico_frame f = {};
  f.transport_hdr = (uint8_t *) (&H);
  H.seq = long_be(seq);
  return RB_FIND(pico_segment_pool, &tq->pool, &f);
}

static struct pico_frame *first_segment(struct pico_tcp_queue *tq)
{
  return RB_MIN(pico_segment_pool, &tq->pool);
}

static struct pico_frame *next_segment(struct pico_tcp_queue *tq, struct pico_frame *cur)
{
  if (!cur)
    return NULL;
  return peek_segment(tq, SEQN(cur) + cur->payload_len);
}


static int pico_enqueue_segment(struct pico_tcp_queue *tq, struct pico_frame *f)
{
  if ((tq->size + f->payload_len) > tq->max_size)
    return 0;
  RB_INSERT(pico_segment_pool, &tq->pool, f);
  tq->size += f->payload_len;
  if (f->payload_len > 0)
    tq->frames++;
  dbg("Queue size: %u/%u frames: %u (Enq: %08x)\n", tq->size, tq->max_size, tq->frames, SEQN(f));
  return f->payload_len;
}

static void pico_discard_segment(struct pico_tcp_queue *tq, struct pico_frame *f)
{
  RB_REMOVE(pico_segment_pool, &tq->pool, f);
  tq->size -= f->payload_len;
  if (f->payload_len > 0)
    tq->frames--;
  dbg("DISCARDED: %08x\n", SEQN(f));
  pico_frame_discard(f);
}



/* Structure for TCP socket */
struct tcp_sack_block {
  uint32_t left;
  uint32_t right;
  struct tcp_sack_block *next;
};

struct pico_socket_tcp {
  struct pico_socket sock;

  /* Tree/queues */
  struct pico_tcp_queue tcpq_in;
  struct pico_tcp_queue tcpq_out;

  /* tcp_output */
  uint32_t snd_nxt;
  uint32_t snd_last;

  /* congestion control */
  uint32_t avg_rtt;
  uint32_t rttvar;
  uint32_t rto;
  uint32_t in_flight;
  uint8_t  timer_running;
  uint16_t cwnd_counter;
  uint16_t cwnd;
  uint16_t ssthresh;
  uint16_t rwnd;
  uint16_t rwnd_scale;

  /* tcp_input */
  uint32_t rcv_nxt;
  uint32_t rcv_ackd;
  uint32_t rcv_processed;
  uint16_t wnd;
  uint16_t wnd_scale;

  /* options */
  uint32_t ts_nxt;
  uint16_t mss;
  uint8_t sack_ok;
  uint8_t ts_ok;
  uint8_t mss_ok;
  uint8_t scale_ok;
  struct tcp_sack_block *sacks;

  /* Transmission */
  uint8_t  x_mode;
  uint8_t  dupacks;
  uint8_t  backoff;

};


/* Queues */
static struct pico_queue in = {};
static struct pico_queue out = {};



/* Useful for getting rid of the beginning of the buffer (read() op) */
static int release_until(struct pico_tcp_queue *q, uint32_t seq)
{
  struct pico_frame *head = first_segment(q);
  int ret = 0;
  while (head && (seq_compare(SEQN(head) + head->payload_len, seq) <= 0)) {
    struct pico_frame *cur = head;
    head = next_segment(q, cur);
    pico_discard_segment(q, cur);
    ret++;
  }
  return ret;
}

static int release_all_until(struct pico_tcp_queue *q, uint32_t seq)
{
  struct pico_frame *f = NULL, *tmp;
  int ret = 0;
  RB_FOREACH_SAFE(f, pico_segment_pool, &q->pool, tmp) {
    if (seq_compare(SEQN(f) + f->payload_len, seq) <= 0) {
      dbg("%s...Discarded One: %08x\n", __FUNCTION__, SEQN(f));
      pico_discard_segment(q, f);
      ret++;
    } else
      return ret;
  }
  return ret;
}

/* API calls */

struct __attribute__((packed)) tcp_pseudo_hdr_ipv4
{
  struct pico_ip4 src;
  struct pico_ip4 dst;
  uint16_t tcp_len;
  uint8_t res;
  uint8_t proto;
};

int pico_tcp_checksum_ipv4(struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  struct pico_socket *s = f->sock;
  struct tcp_pseudo_hdr_ipv4 pseudo;
  if (!hdr || !s)
    return -1;

  pseudo.src.addr = s->local_addr.ip4.addr;
  pseudo.dst.addr = s->remote_addr.ip4.addr;
  pseudo.res = 0;
  pseudo.proto = PICO_PROTO_TCP;
  pseudo.tcp_len = short_be(f->transport_len);

  hdr->crc = 0;
  hdr->crc = pico_dualbuffer_checksum(&pseudo, sizeof(struct tcp_pseudo_hdr_ipv4), hdr, f->transport_len);
  hdr->crc = short_be(hdr->crc);
  return 0;
}

static int pico_tcp_process_out(struct pico_protocol *self, struct pico_frame *f)
{
  pico_network_send(f);
  return 0;
}

int pico_tcp_push(struct pico_protocol *self, struct pico_frame *data);

/* Interface: protocol definition */
struct pico_protocol pico_proto_tcp = {
  .name = "tcp",
  .proto_number = PICO_PROTO_TCP,
  .layer = PICO_LAYER_TRANSPORT,
  .process_in = pico_transport_process_in,
  .process_out = pico_tcp_process_out,
  .push = pico_tcp_push,
  .q_in = &in,
  .q_out = &out,
};

static uint32_t pico_paws(void)
{
  static unsigned long _paws = 0;
  //_paws = pico_tick +  0xc0cac01a + _paws;
  return long_be(_paws); /*XXX: implement paws */
}

static void tcp_add_options(struct pico_socket_tcp *ts, struct pico_frame *f, uint16_t flags, int optsiz)
{
  uint32_t tsval = long_be(pico_tick);
  uint32_t tsecr = long_be(ts->ts_nxt);
  int i = 0;
  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;

  memset(f->start, PICO_TCP_OPTION_NOOP, optsiz); /* fill blanks with noop */

  if (flags & PICO_TCP_SYN) { 
    f->start[i++] = PICO_TCP_OPTION_MSS;
    f->start[i++] = PICO_TCPOPTLEN_MSS;
    f->start[i++] = (ts->mss >> 8) & 0xFF;
    f->start[i++] = ts->mss & 0xFF;
    f->start[i++] = PICO_TCP_OPTION_SACK_OK;
    f->start[i++] = PICO_TCPOPTLEN_SACK_OK;
  }

  f->start[i++] = PICO_TCP_OPTION_WS;
  f->start[i++] = PICO_TCPOPTLEN_WS;
  f->start[i++] = ts->wnd_scale;

  if (optsiz >= 12) {
    f->start[i++] = PICO_TCP_OPTION_TIMESTAMP;
    f->start[i++] = PICO_TCPOPTLEN_TIMESTAMP;
    memcpy(f->start + i, &tsval, 4);
    i += 4;
    memcpy(f->start + i, &tsecr, 4);
    i += 4;
  }

  if (flags & PICO_TCP_ACK) {
    struct tcp_sack_block *sb;
    int len_off;

    if (ts->sack_ok && ts->sacks) {
      f->start[i++] = PICO_TCP_OPTION_SACK;
      len_off = i;
      f->start[i++] = PICO_TCPOPTLEN_SACK;
      while(ts->sacks) {
        sb = ts->sacks;
        ts->sacks = sb->next;
        memcpy(f->start + i, sb, 2 * sizeof(uint32_t));
        i += (2 * sizeof(uint32_t));
        f->start[len_off] += (2 * sizeof(uint32_t));
        pico_free(sb);
      }
    }
  }
  if (i < optsiz)
    f->start[ optsiz - 1 ] = PICO_TCP_OPTION_END;
}

static void tcp_set_space(struct pico_socket_tcp *t)
{
  int mtu, space;
  int shift = 0;

  mtu = t->mss + PICO_SIZE_TCPHDR + PICO_SIZE_TCPOPT_SYN ;
  if (t->tcpq_in.max_size == 0) {
    space = 1024 * 1024 * 1024; /* One Gigabyte, for unlimited sockets. */
  } else {
    space = ((t->tcpq_in.max_size - t->tcpq_in.size) / mtu) * t->mss;
  }
  if (space < 0)
    space = 0;
  while(space > 0xFFFF) {
    space >>= 1;
    shift++;
  }
  dbg("\n\nSPACE: %d\n\n", space);
  if ((space == 0) || (t->wnd_scale == 0) || (shift != t->wnd_scale) || ((space - t->wnd) > (space>>2))) {
    t->wnd = space;
    t->wnd_scale = shift;
  }
}


/* Return 32-bit aligned option size */
static int tcp_options_size(struct pico_socket_tcp *t, uint16_t flags)
{
  int size = 0;
  struct tcp_sack_block *sb = t->sacks;

  if (flags & PICO_TCP_SYN) {  /* Full options */ 
    size = PICO_TCPOPTLEN_MSS + PICO_TCP_OPTION_SACK_OK + PICO_TCPOPTLEN_WS + PICO_TCPOPTLEN_TIMESTAMP;
  } else {

   /* Always update window scale. */
    size += PICO_TCPOPTLEN_WS;

    if (t->ts_ok)
      size += PICO_TCPOPTLEN_TIMESTAMP;

    size+= PICO_TCPOPTLEN_END;
  }
  if ((flags & PICO_TCP_ACK) && (t->sack_ok && sb)) {
    size += 2;
    while(sb) {
      size += (2 * sizeof(uint32_t));
      sb = sb->next;
    }
  }
  size = (((size + 3) >> 2) << 2);
  return size;
}

int pico_tcp_overhead(struct pico_socket *s)
{
  if (!s)
    return 0;

  return PICO_SIZE_TCPHDR + tcp_options_size((struct pico_socket_tcp *)s, 0); /* hdr + Options size for data pkt */

}

static void tcp_process_sack(struct pico_socket_tcp *t, uint32_t start, uint32_t end)
{
  struct pico_frame *f, *tmp; 
  int cmp;
  dbg("Processing sack %08x-%08x...\n", start, end);
  RB_FOREACH_SAFE(f, pico_segment_pool, &t->tcpq_out.pool, tmp) {
    dbg("...found segment at %08x\n", SEQN(f));
    cmp = seq_compare(SEQN(f), start);
    dbg("Cmp: %d\n", cmp);
    if (cmp > 0) 
      return;

    if (cmp == 0) {
      cmp = seq_compare(SEQN(f) + f->payload_len, end);
      if (cmp > 0) {
        dbg("Invalid SACK: ignoring.\n");
      }

      dbg("%s...Discarded One: %08x\n", __FUNCTION__, SEQN(f));
      pico_discard_segment(&t->tcpq_out, f);

      if (cmp == 0) {
        /* that was last segment sacked. Job done */
        return;
      }
    }
  }
}

static void tcp_rcv_sack(struct pico_socket_tcp *t, uint8_t *opt, int len)
{
  uint32_t *start, *end;
  int i = 0;
  if (len % 8) {
    dbg("SACK: Invalid len.\n");
    return;
  }
  while (i < len) {
    start = (uint32_t *)(opt + i);
    i += 4;
    end = (uint32_t *)(opt + i);
    i += 4;
    tcp_process_sack(t, long_be(*start), long_be(*end));
  }
}

static void tcp_parse_options(struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)f->sock;
  uint8_t *opt = f->transport_hdr + PICO_SIZE_TCPHDR;
  int i = 0;
  while (i < (f->transport_len - PICO_SIZE_TCPHDR)) {
    uint8_t type =  opt[i++];
    uint8_t len =  opt[i++];
    if (f->payload && ((opt + i) > f->payload))
      break;
    switch (type) {
      case PICO_TCP_OPTION_NOOP:
      case PICO_TCP_OPTION_END:
        i--; /* unread len */
        break;
      case PICO_TCP_OPTION_WS:
        if (len != PICO_TCPOPTLEN_WS) {
          dbg("TCP Window scale: bad len received.\n");
          i += len - 2;
          break;
        }
        t->rwnd_scale = opt[i++];
        break;
      case PICO_TCP_OPTION_SACK_OK:
        if (len != PICO_TCPOPTLEN_SACK_OK) {
          dbg("TCP option sack: bad len received.\n");
          i += len - 2;
          break;
        }
        t->sack_ok = 1;
        break;
      case PICO_TCP_OPTION_MSS: {
        uint16_t *mss;
        if (len != PICO_TCPOPTLEN_MSS) {
          dbg("TCP option mss: bad len received.\n");
          i += len - 2;
          break;
        }
        t->mss_ok = 1;
        mss = (uint16_t *)(opt + i);
        i += sizeof(uint16_t);
        if (t->mss > short_be(*mss))
          t->mss = short_be(*mss);
        break;
      }
      case PICO_TCP_OPTION_TIMESTAMP: {
        uint32_t *tsval, *tsecr;
        if (len != PICO_TCPOPTLEN_TIMESTAMP) {
          dbg("TCP option timestamp: bad len received.\n");
          i += len - 2;
          break;
        }
        t->ts_ok = 1;
        tsval = (uint32_t *)(opt + i);
        i += sizeof(uint32_t);
        tsecr = (uint32_t *)(opt + i);
        f->timestamp = long_be(*tsecr);
        i += sizeof(uint32_t);

        t->ts_nxt = long_be(*tsval);
        break;
      }
      case PICO_TCP_OPTION_SACK:
      {
        dbg("TCP option SACK received.\n");
        tcp_rcv_sack(t, opt + i, len - 2);
        i += len - 2;
        break;
      }
      default:
        dbg("TCP: received unsupported option %u\n", type);
        i += len - 2;
    }
  }
}


static int tcp_send(struct pico_socket_tcp *ts, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr= (struct pico_tcp_hdr *) f->transport_hdr;
  uint32_t next_to_send;
  struct pico_socket *s = (struct pico_socket *)ts;
  hdr->trans.sport = ts->sock.local_port;
  hdr->trans.dport = ts->sock.remote_port;
  hdr->seq = long_be(ts->snd_nxt);

  if (ts->rcv_nxt != 0) {
    if ( (ts->rcv_ackd == 0) || (seq_compare(ts->rcv_ackd, ts->rcv_nxt) != 0) || (hdr->flags & PICO_TCP_ACK)) {
      hdr->flags |= PICO_TCP_ACK;
      hdr->ack = long_be(ts->rcv_nxt);
      ts->rcv_ackd = ts->rcv_nxt;
    }
  }

  next_to_send = ts->snd_nxt;

  if (hdr->flags & PICO_TCP_SYN)
    next_to_send++;
  if (f->payload_len > 0) {
    next_to_send = SEQN(f) + f->payload_len;
    hdr->flags |= PICO_TCP_PSH;
  }


  if (seq_compare(next_to_send, ts->snd_nxt) > 0) {
    ts->snd_nxt = next_to_send;
  }

  f->start = f->transport_hdr + PICO_SIZE_TCPHDR;
  hdr->rwnd = short_be(ts->wnd);
  pico_tcp_checksum_ipv4(f);
  /* TCP: ENQUEUE to PROTO ( Transmit ) */
  pico_enqueue(&out, pico_frame_copy(f));
  ts->in_flight++;
  dbg("DBG> [tcp output] socket: %p state: %02x --> local port:%d remote port: %d seq: %08x ack: %08x flags: %02x = t_len: %d, hdr: %u payload: %d\n",
      s, TCPSTATE(s) >> 8, short_be(hdr->trans.sport), short_be(hdr->trans.dport), SEQN(f), ACKN(f), hdr->flags, f->transport_len, hdr->len >> 2, f->payload_len );
  return 0;
}

struct pico_socket *pico_tcp_open(void)
{
  struct pico_socket_tcp *t = pico_zalloc(sizeof(struct pico_socket_tcp));
  if (!t)
    return NULL;
  t->mss = PICO_TCP_DEFAULT_MSS;
  return &t->sock;
}


int pico_tcp_read(struct pico_socket *s, void *buf, int len)
{
  struct pico_socket_tcp *t = TCP_SOCK(s);
  struct pico_frame *f;
  uint32_t in_frame_off, in_frame_len;
  int tot_rd_len = 0;


  while (tot_rd_len < len) {
    /* To be sure we don't have garbage at the beginning */
    release_until(&t->tcpq_in, t->rcv_processed);
    f = first_segment(&t->tcpq_in);
    if (!f)
      return tot_rd_len;

    /* Hole at the beginning of data, awaiting retransmissions. */
    if(seq_compare(t->rcv_processed, SEQN(f)) < 0) {
      return tot_rd_len;
    }

    if(seq_compare(t->rcv_processed, SEQN(f)) > 0) {
      in_frame_off = t->rcv_processed - SEQN(f);
      in_frame_len = f->payload_len - in_frame_off;
    } else {
      in_frame_off = 0;
      in_frame_len = f->payload_len;
    }

    if ((in_frame_len + tot_rd_len) > len) {
      in_frame_len = len - tot_rd_len;
    }

    memcpy(buf + tot_rd_len, f->payload + in_frame_off, in_frame_len);
    tot_rd_len += in_frame_len;
    t->rcv_processed += in_frame_len;

    if ((in_frame_len == 0) || (in_frame_len == f->payload_len)) {
      pico_discard_segment(&t->tcpq_in, f);
    }
    tcp_set_space(t);
  }
  return tot_rd_len;
}

int pico_tcp_initconn(struct pico_socket *s)
{
  struct pico_socket_tcp *ts = TCP_SOCK(s);
  struct pico_frame *syn;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(ts, PICO_TCP_SYN);

  syn = s->net->alloc(s->net, PICO_SIZE_TCPHDR + opt_len);
  if (!syn)
    return -1;
  hdr = (struct pico_tcp_hdr *) syn->transport_hdr;

  ts->snd_nxt = long_be(pico_paws());
  ts->snd_last = ts->snd_nxt;
  syn->sock = s;
  hdr->seq = long_be(ts->snd_nxt);
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2;
  hdr->flags = PICO_TCP_SYN;
  hdr->rwnd = short_be(ts->wnd);
  tcp_set_space(ts);
  tcp_add_options(ts,syn, PICO_TCP_SYN, opt_len);
  tcp_send(ts, syn);
  pico_frame_discard(syn);
  return 0;
}


static int tcp_send_synack(struct pico_socket *s)
{
  struct pico_socket_tcp *ts = TCP_SOCK(s);
  struct pico_frame *synack;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(ts, PICO_TCP_SYN | PICO_TCP_ACK);


  synack = s->net->alloc(s->net, PICO_SIZE_TCPHDR + opt_len);
  if (!synack)
    return -1;
  hdr = (struct pico_tcp_hdr *) synack->transport_hdr;

  synack->sock = s;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2;
  hdr->flags = PICO_TCP_SYN | PICO_TCP_ACK;
  hdr->rwnd = short_be(ts->wnd);
  hdr->seq = short_be(ts->snd_nxt);
  ts->rcv_processed = long_be(hdr->seq);
  ts->snd_last = ts->snd_nxt + 1;
  tcp_set_space(ts);
  tcp_add_options(ts,synack, hdr->flags, opt_len);
  synack->payload_len = 0;
  synack->timestamp = pico_tick;
  //pico_enqueue_segment(&s->tcpq_out,synack);
  tcp_send(ts, synack);
  pico_frame_discard(synack);
  return 0;
}


static void tcp_send_ack(struct pico_socket_tcp *t)
{
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr;
  int opt_len = tcp_options_size(t, PICO_TCP_ACK);

  f = t->sock.net->alloc(t->sock.net, PICO_SIZE_TCPHDR + opt_len);
  if (!f) {
    return;
  }
  f->sock = &t->sock;
  hdr= (struct pico_tcp_hdr *) f->transport_hdr;
  hdr->len = (PICO_SIZE_TCPHDR + opt_len) << 2;
  hdr->flags = PICO_TCP_ACK;
  hdr->rwnd = short_be(t->wnd);
  tcp_set_space(t);
  tcp_add_options(t,f, PICO_TCP_ACK, opt_len);
  tcp_send(t,f);

}

static void tcp_sack_prepare(struct pico_socket_tcp *t)
{
  struct pico_frame *pkt;
  uint32_t left=0, right=0;
  struct tcp_sack_block *sb;
  int n = 0;
  if (t->sacks) /* previous sacks are pending */
    return;

  pkt = first_segment(&t->tcpq_in);
  while(n < 3) {
    if (!pkt) {
      if(left) {
        sb = pico_zalloc(sizeof(struct tcp_sack_block));
        sb->left = long_be(left);
        sb->right = long_be(right);
        n++;
        sb->next = t->sacks;
        t->sacks = sb;
        left = 0;
        right = 0;
      }
      break;
    }
    if ((SEQN(pkt) < t->rcv_nxt)) {
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    }
    if (!left) {
      left = SEQN(pkt);
      right = SEQN(pkt) + pkt->payload_len;
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    }
    if(SEQN(pkt) == (right + 1)) {
      right += pkt->payload_len;
      pkt = next_segment(&t->tcpq_in, pkt);
      continue;
    } else {
      sb = pico_zalloc(sizeof(struct tcp_sack_block));
      if (!sb)
        break;
      sb->left = long_be(left);
      sb->right = long_be(right);
      n++;
      sb->next = t->sacks;
      t->sacks = sb;
      left = 0;
      right = 0;
      pkt = next_segment(&t->tcpq_in, pkt);
    }
  }
}

static int tcp_data_in(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  if ((hdr->len >> 2) < f->transport_len) {
    tcp_parse_options(f);
    f->payload = f->transport_hdr + (hdr->len >>2);
    f->payload_len = f->transport_len - (hdr->len >>2);
    if (seq_compare(SEQN(f) + f->payload_len, t->rcv_nxt) > 0) {
      pico_enqueue_segment(&t->tcpq_in, f);
      if (seq_compare(SEQN(f), t->rcv_nxt) == 0) {
        struct pico_frame *nxt;
        dbg("TCP> exactly what I was expecting!\n");
        t->rcv_nxt = SEQN(f) + f->payload_len;
        nxt = peek_segment(&t->tcpq_in, t->rcv_nxt);
        while(nxt) {
          dbg("scrolling rcv_nxt...%08x\n", t->rcv_nxt);
          t->rcv_nxt += f->payload_len;
          nxt = peek_segment(&t->tcpq_in, t->rcv_nxt);
        }
        if (t->sock.wakeup) {
          t->sock.wakeup(PICO_SOCK_EV_RD, &t->sock);
        }
      } else {
        dbg("TCP> hi segment. Possible packet loss. I'll dupack this. (exp: %x got: %x)\n", t->rcv_nxt, SEQN(f));
        if (t->sack_ok) {
          tcp_sack_prepare(t);
        }
      }

    }
    /* In either case, ack til recv_nxt. */
    if (t->sock.state != PICO_SOCKET_STATE_TCP_CLOSE_WAIT)
      tcp_send_ack(t);
    return 0;
  } else {
    return -1;
  }
}

static int tcp_ack_advance_una(struct pico_socket_tcp *t, struct pico_frame *f)
{
  int ret =  release_all_until(&t->tcpq_out, ACKN(f));
  return ret;
}

static uint16_t time_diff(unsigned long a, unsigned long b)
{
  if (a >= b)
    return (a - b);
  else
    return (b - a);
}

static int fresh_ack(struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)f->sock;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  if (hdr->flags & PICO_TCP_ACK) {
    if (seq_compare(ACKN(f), SEQN(first_segment(&t->tcpq_out))) > 0) {
      return 1;
    }
  }
  return 0;
}

static void tcp_rtt(struct pico_socket_tcp *t, uint32_t rtt)
{

  uint32_t avg = t->avg_rtt;
  uint32_t rvar = t->rttvar;
  if (!avg) {
    /* This follows RFC2988
     * (2.2) When the first RTT measurement R is made, the host MUST set
     *
     * SRTT <- R
     * RTTVAR <- R/2
     * RTO <- SRTT + max (G, K*RTTVAR)
     */
    t->avg_rtt = rtt;
    t->rttvar = rtt >> 1;
    t->rto = t->avg_rtt + (t->rttvar << 4);
  } else {
    int var = (t->avg_rtt - rtt);
    if (var < 0)
      var = 0-var;
    /* RFC2988, section (2.3). Alpha and beta are the ones suggested. */

    /* First, evaluate a new value for the rttvar */
    t->rttvar <<= 2;
    t->rttvar -= rvar;
    t->rttvar += var;
    t->rttvar >>= 2;

    /* Then, calculate the new avg_rtt */
    t->avg_rtt <<= 3;
    t->avg_rtt -= avg;
    t->avg_rtt += rtt;
    t->avg_rtt >>= 3;

    /* Finally, assign a new value for the RTO, as specified in the RFC, with K=4 */
    t->rto = t->avg_rtt + (t->rttvar << 2);
  }
  dbg(" -----=============== RTT AVG: %u RTTVAR: %u RTO: %u ======================----\n", t->avg_rtt, t->rttvar, t->rto);
}

static void tcp_congestion_control(struct pico_socket_tcp *t)
{
  if (t->in_flight < t->cwnd)
    return;

  if (t->cwnd < t->ssthresh) {
    t->cwnd++;
  } else {
    t->cwnd_counter++;
    if (t->cwnd_counter >= t->cwnd) {
      t->cwnd++;
      t->cwnd_counter -= t->cwnd;
    }
  }
  dbg("TCP CONG> cwnd is now %u\n", t->cwnd);
}

static void tcp_retrans_timeout(unsigned long val, void *sock)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) sock;
  struct pico_frame *f = NULL;
  unsigned long limit = val - t->rto;
  struct pico_tcp_hdr *hdr;
  unsigned long next_limit = 0;

  /* was timer cancelled? */
  if (!t->timer_running)
    return;

  dbg("TIMEOUT! backoff = %d\n", t->backoff);

  f = first_segment(&t->tcpq_out);
  while (f) {
    if (f->timestamp <= limit) {
      hdr = (struct pico_tcp_hdr *)f->transport_hdr;
      dbg("TCP BLACKOUT> TIMED OUT (output) frame %08x, len= %d\n", SEQN(f), f->payload_len);
      t->x_mode = PICO_TCP_BLACKOUT;
      t->cwnd = PICO_TCP_IW;
      t->in_flight = 0;
      f->timestamp = pico_tick;
      tcp_add_options(t, f, 0, f->transport_len - f->payload_len - PICO_SIZE_TCPHDR);
      hdr->rwnd = short_be(t->wnd);
      pico_tcp_checksum_ipv4(f);
      /* TCP: ENQUEUE to PROTO ( retransmit )*/
      pico_enqueue(&out, pico_frame_copy(f));
      t->backoff++;
      pico_timer_add((t->rto << t->backoff), tcp_retrans_timeout, t);
      return;
    } else {
      if ((next_limit == 0) || (next_limit < f->timestamp)) {
        next_limit = f->timestamp;
      }
    }
    f = next_segment(&t->tcpq_out, f);
  }
  if (next_limit > 0) {
    pico_timer_add(t->rto - (val - next_limit), tcp_retrans_timeout, t);
  }
  t->backoff = 0;
  t->timer_running = 0;
}


static int tcp_retrans(struct pico_socket_tcp *t)
{
  struct pico_frame *f = NULL;
  struct pico_tcp_hdr *hdr;
  f = first_segment(&t->tcpq_out);
  if (f) {
    hdr = (struct pico_tcp_hdr *)f->transport_hdr;
    dbg("TCP> RETRANS (by dupack) frame %08x, len= %d\n", SEQN(f), f->payload_len);
    f->timestamp = pico_tick;
    tcp_add_options(t, f, 0, f->transport_len - f->payload_len - PICO_SIZE_TCPHDR);
    hdr->rwnd = short_be(t->wnd);
    pico_tcp_checksum_ipv4(f);
    /* TCP: ENQUEUE to PROTO ( retransmit )*/
    pico_enqueue(&out, pico_frame_copy(f));
    pico_timer_add((t->rto), tcp_retrans_timeout, t);
    t->in_flight++;
    return(f->payload_len);
  }
  return 0;
}

static int tcp_ack(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) f->transport_hdr;
  uint32_t rtt = 0;
  int acked = 0;
  if ((hdr->flags & PICO_TCP_ACK) == 0)
    return -1;

  tcp_parse_options(f);


  if (fresh_ack(f)) {
    struct pico_frame *una = first_segment(&t->tcpq_out);
    t->x_mode = PICO_TCP_LOOKAHEAD;
    t->backoff = 0;
    acked = tcp_ack_advance_una(t, f);
    dbg("TCP ACK> FRESH ACK %08x (acked %d) Queue size: %u/%u frames: %u cwnd: %u in_flight: %u snd_una: %u\n", ACKN(f), acked, t->tcpq_out.size, t->tcpq_out.max_size, t->tcpq_out.frames, t->cwnd, t->in_flight, SEQN(una));

    /* Do rtt/rttvar/rto calculations */
    if(una && (una->timestamp != 0)) {
      rtt = time_diff(pico_tick, una->timestamp);
      if (rtt)
        tcp_rtt(t, rtt);
    }

    /* Do congestion control */
    tcp_congestion_control(t);

    if (acked > t->in_flight) {
      dbg("WARNING: in flight < 0\n");
      t->in_flight = 0;
    } else
      t->in_flight -= acked;

  } else {
    if (t->in_flight > 0)
      t->in_flight--;
    if (t->x_mode < PICO_TCP_RECOVER) {
      /* One should be acked. */
      t->x_mode++;
      dbg("\n\n\n\nTCP RETRANSMIT> DUPACK:%d! snd_una: %08x, snd_nxt: %08x, acked now: %08x\n", t->x_mode, SEQN(first_segment(&t->tcpq_out)), t->snd_nxt, ACKN(f));
      if (t->in_flight < t->cwnd)
        tcp_retrans(t);
      t->ssthresh = (t->cwnd >> 1);
    } else {
      dbg("\n\n\n\nTCP RECOVER> DUPACK! snd_una: %08x, snd_nxt: %08x, acked now: %08x\n", SEQN(first_segment(&t->tcpq_out)), t->snd_nxt, ACKN(f));
      if (t->in_flight < t->cwnd)
        tcp_retrans(t);
      if (t->cwnd > t->ssthresh)
        t->cwnd--;
    }
  }
  return 0;
}

static int tcp_syn(struct pico_socket *s, struct pico_frame *f)
{
  /* TODO: Check against backlog length */
  struct pico_socket_tcp *new = (struct pico_socket_tcp *)pico_socket_clone(s);
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *)f->transport_hdr;
  if (!new)
    return -1;
  new->sock.remote_port = ((struct pico_trans *)f->transport_hdr)->sport;
#ifdef PICO_SUPPORT_IPV4
  if (IS_IPV4(f))
    new->sock.remote_addr.ip4.addr = ((struct pico_ipv4_hdr *)(f->net_hdr))->src.addr;
#endif
#ifdef PICO_SUPPORT_IPV6
  if (IS_IPV4(f))
    memcpy(new->sock.remote_addr.ip6.addr, ((struct pico_ipv6_hdr *)(f->net_hdr))->src, PICO_SIZE_IP6);
#endif

  /* Set socket limits */
  new->tcpq_in.max_size = PICO_DEFAULT_SOCKETQ;
  new->tcpq_out.max_size = PICO_DEFAULT_SOCKETQ;

  f->sock = &new->sock;
  tcp_parse_options(f);
  new->mss = PICO_TCP_DEFAULT_MSS;
  new->rcv_nxt = long_be(hdr->seq) + 1;
  new->snd_nxt = long_be(pico_paws());
  new->snd_last = new->snd_nxt;
  new->cwnd = PICO_TCP_IW;
  new->ssthresh = 40;
  new->rwnd = short_be(hdr->rwnd);
  new->sock.parent = s;
  new->sock.wakeup = s->wakeup;
  /* Initialize timestamp values */
  new->sock.state = PICO_SOCKET_STATE_BOUND | PICO_SOCKET_STATE_CONNECTED | PICO_SOCKET_STATE_TCP_SYN_RECV;
  pico_socket_add(&new->sock);
  tcp_send_synack(&new->sock);
  dbg("SYNACK sent, socket added.\n");
  return 0;
}

static void tcp_set_init_point(struct pico_socket *s)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  t->rcv_processed = t->rcv_nxt;
}

static int tcp_synack(struct pico_socket *s, struct pico_frame *f)
{
  return 0;
}

static int tcp_first_ack(struct pico_socket *s, struct pico_frame *f)
{
  if (fresh_ack(f)) {
    s->state &= 0x00FF;
    s->state |= PICO_SOCKET_STATE_TCP_ESTABLISHED;
    tcp_set_init_point(s);
    tcp_ack(s, f);
    tcp_data_in(s, f);
    dbg("TCP: Established.\n");
    if (s->parent && s->parent->wakeup) {
      s->parent->wakeup(PICO_SOCK_EV_CONN, s->parent);
    }
    return 0;
  } else {
    //tcp_send_synack(s);
    return 0;
  }
}



static int tcp_closewait(struct pico_socket *s, struct pico_frame *f)
{
  s->state &= 0x00FF;
  s->state |= PICO_SOCKET_STATE_TCP_CLOSE_WAIT;
  if (f->payload_len > 0)
    tcp_data_in(s,f);
  if (f->flags & PICO_TCP_ACK)
    tcp_ack(s,f);
  return 0;
}

static int tcp_fin(struct pico_socket *s, struct pico_frame *f)
{
  return 0;
}

static int tcp_finack(struct pico_socket *s, struct pico_frame *f)
{
  return 0;
}

static int tcp_rst(struct pico_socket *s, struct pico_frame *f)
{
  return 0;
}



struct tcp_action_entry {
  uint16_t tcpstate;
  int (*syn)(struct pico_socket *s, struct pico_frame *f);
  int (*synack)(struct pico_socket *s, struct pico_frame *f);
  int (*ack)(struct pico_socket *s, struct pico_frame *f);
  int (*data)(struct pico_socket *s, struct pico_frame *f);
  int (*fin)(struct pico_socket *s, struct pico_frame *f);
  int (*finack)(struct pico_socket *s, struct pico_frame *f);
  int (*rst)(struct pico_socket *s, struct pico_frame *f);
};

static struct tcp_action_entry tcp_fsm[] = {
    /* State                            syn              synack       ack             data          fin             finack          rst*/
  { PICO_SOCKET_STATE_TCP_UNDEF,        NULL,            NULL,        NULL,           NULL,         NULL,           NULL,           NULL     },
  { PICO_SOCKET_STATE_TCP_CLOSED,       NULL,            NULL,        NULL,           NULL,         NULL,           NULL,           NULL     },
  { PICO_SOCKET_STATE_TCP_LISTEN,       &tcp_syn,        NULL,        NULL,           NULL,         NULL,           NULL,           NULL     },
  { PICO_SOCKET_STATE_TCP_SYN_SENT,     NULL,            &tcp_synack, NULL,           NULL,         NULL,           NULL,           &tcp_rst },
  { PICO_SOCKET_STATE_TCP_SYN_RECV,     NULL,            NULL,        &tcp_first_ack, NULL,         NULL,           NULL,           &tcp_rst },
  { PICO_SOCKET_STATE_TCP_ESTABLISHED,  NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_closewait, &tcp_closewait, &tcp_rst },
  { PICO_SOCKET_STATE_TCP_CLOSE_WAIT,   NULL,            NULL,        &tcp_ack,       NULL,         NULL,           &tcp_ack,       &tcp_rst },
  { PICO_SOCKET_STATE_TCP_LAST_ACK,     NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_fin,       &tcp_finack,    &tcp_rst },
  { PICO_SOCKET_STATE_TCP_FIN_WAIT1,    NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_fin,       &tcp_finack,    &tcp_rst },
  { PICO_SOCKET_STATE_TCP_FIN_WAIT2,    NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_fin,       &tcp_finack,    &tcp_rst },
  { PICO_SOCKET_STATE_TCP_CLOSING,      NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_fin,       &tcp_finack,    &tcp_rst },
  { PICO_SOCKET_STATE_TCP_TIME_WAIT,    NULL,            NULL,        &tcp_ack,       &tcp_data_in, &tcp_fin,       &tcp_finack,    &tcp_rst }
};

int pico_tcp_input(struct pico_socket *s, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *) (f->transport_hdr);
  int ret = -1;
  uint8_t flags = hdr->flags;
  struct tcp_action_entry *action = &tcp_fsm[s->state >> 8];

  if (!hdr)
    goto discard;

  f->payload = (f->transport_hdr + (hdr->len>>2));
  f->payload_len = f->transport_len - (hdr->len >> 2);

  dbg("[%lu] TCP> [tcp input] socket: %p state: %d <-- local port:%d remote port: %d seq: %08x ack: %08x flags: %02x = t_len: %d, hdr: %u payload: %d\n", pico_tick,
      s, s->state >> 8, short_be(hdr->trans.dport), short_be(hdr->trans.sport), SEQN(f), ACKN(f), hdr->flags, f->transport_len, hdr->len >> 2, f->payload_len );

  /* This copy of the frame has the current socket as owner */
  f->sock = s;

  /* Those are not supported at this time. */
  flags &= ~(PICO_TCP_CWR | PICO_TCP_URG | PICO_TCP_ECN);
  if (flags == PICO_TCP_SYN) {
    if (action->syn)
      action->syn(s,f);
  } else if (flags == (PICO_TCP_SYN | PICO_TCP_ACK)) {
    if (action->synack)
      action->synack(s,f);
  } else {
    if (f->payload_len > 0) {
      if (action->data)
        action->data(s,f);
      flags &= ~PICO_TCP_PSH;
    }
    if (flags == PICO_TCP_FIN) {
      if (action->fin)
        action->fin(s,f);
    }
    if (flags == (PICO_TCP_FIN | PICO_TCP_ACK)) {
      if (action->finack)
        action->finack(s,f);
    }
    if (flags & PICO_TCP_RST) {
      if (action->rst)
        action->rst(s,f);
    }
    if (flags == PICO_TCP_ACK) {
      if (action->ack)
        action->ack(s,f);
    }
  }

discard:
  pico_frame_discard(f);
  return ret;
}


int pico_tcp_output(struct pico_socket *s, int loop_score)
{
  struct pico_socket_tcp *t = (struct pico_socket_tcp *)s;
  struct pico_frame *f;
  struct pico_tcp_hdr *hdr; 
  int sent = 0;

  f = peek_segment(&t->tcpq_out, t->snd_nxt + 1);
  while(f && (t->in_flight <= t->cwnd)) {
    hdr = (struct pico_tcp_hdr *)f->transport_hdr;
    dbg("TCP> DEQUEUED (for output) frame %08x, len= %d\n", SEQN(f), f->payload_len);
    f->timestamp = pico_tick;
    tcp_add_options(t, f, hdr->flags, tcp_options_size(t, hdr->flags));
    tcp_send(t, f);
    sent++;
    loop_score--;
    f = peek_segment(&t->tcpq_out, t->snd_nxt + 1);
#if 0
    if (!f && (t->x_mode > PICO_TCP_LOOKAHEAD)) {
      f = first_segment(&t->tcpq_out); /* Try to restart the queue when in retransmission */
      if (f)
        t->snd_nxt = SEQN(f) + f->payload_len;
    }
#endif
  }
  if (sent > 0) {
    if (t->rto < PICO_TCP_RTO_MIN)
      t->rto = PICO_TCP_RTO_MIN;
    if (!t->timer_running) {
      pico_timer_add(t->rto, tcp_retrans_timeout, t);
      t->timer_running++;
    }
  }
  return loop_score;
}

int pico_tcp_push(struct pico_protocol *self, struct pico_frame *f)
{
  struct pico_tcp_hdr *hdr = (struct pico_tcp_hdr *)f->transport_hdr;
  struct pico_socket_tcp *t = (struct pico_socket_tcp *) f->sock;
  hdr->seq = long_be(t->snd_last + 1);
  hdr->len = (f->payload - f->transport_hdr) << 2;
  hdr->trans.sport = t->sock.local_port;
  hdr->trans.dport = t->sock.remote_port;
  if (pico_enqueue_segment(&t->tcpq_out,f) > 0) {
    t->snd_last += f->payload_len;
    return f->payload_len;
  }
  else return 0;
}