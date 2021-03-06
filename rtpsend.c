/*
 * (c) 1998-2018 by Columbia University; all rights reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#ifndef WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "notify.h"
#include "rtp.h"
#include "multimer.h"
#include "sysdep.h"

extern int hpt(char*, struct sockaddr_in*, unsigned char*);

static int verbose = 0;
static FILE *in;
static int sock[2];  /* output sockets */
static int loop = 0; /* play file indefinitely if set */


/*
* Node either has a parameter or a non-zero pointer to list.
*/
typedef struct node {
  struct node *next, *list;  /* parameter in list, level down */
  char *type;    /* parameter type */
  unsigned long num;       /* numeric value */
  char *string;  /* string value */
} node_t;


static void usage(char *argv0)
{
  fprintf(stderr,
    "usage: %s [-alv] [-f file] [-s port] address/port[/ttl]\n",
    argv0);
  exit(1);
} /* usage */


/*
* Convert hexadecimal numbers in 'text' to binary in 'buffer'.
* Ignore embedded whitespace.
* Return length in bytes.
*/
static int hex(char *text, char *buffer)
{
  char *s;
  char byte[3];
  int nibble = 0;
  int len = 0;

  byte[2] = '\0';
  for (s = text; *s; s++) {
    if (isspace((int)*s)) continue;

    byte[nibble++] = *s;
    if (nibble == 2) {
      nibble = 0;
      buffer[len++] = strtol(byte, (char **)NULL, 16);
    }
  }
  return len;
} /* hex */


/*
* Convert textual description into parse tree.
* (SDES (ssrc=<ssrc> cname=<cname> ...) (ssrc=<ssrc> ...))
* Return pointer to first node.
*/
static node_t *parse(char *text)
{
  int string = 0;
  int level = 0;
  char tmp[1024];
  int c = 0;
  node_t *n;
  node_t *first = (node_t *)NULL, *last = (node_t *)NULL;
  char *s;

  /* convert to tree */
  for (s = text; *s; s++) {
    if (string) {
      tmp[c++] = *s;
      if (*s == '"') string = 0;
    }
    else if (*s == '(') {
      if (level > 0) tmp[c++] = *s;
      else c = 0;
      level++;
    }
    else if (*s == ')') {
      level--;
      if (level == 0) {
        n = calloc(1, sizeof(node_t));
        /* append node to list */
        if (!first) first = n;
        if (last) last->next = n;
        last = n;
        tmp[c] = 0;
        c = 0;
        /* attach list */
        n->list = parse(tmp);
      }
      else {
        tmp[c++] = *s;
      }
    }
    else if (*s == '"') {
      tmp[c++] = *s;
      string = 1;
    }
    else if (level >= 1) {
      tmp[c++] = *s;
    }
    /* end of parameter/value pair */
    else if (isspace((int)*s)) {
      char *e;

      tmp[c] = '\0';
      if (c > 0) {
        c = 0;
        n = calloc(1, sizeof(node_t));
        if (!first) first = n;
        if (last) last->next = n;
        last = n;
        e = strchr(tmp, '=');
        if (e) {
          char *v = e+1;

          *e = '\0';
          if (isdigit((int)*v)) n->num = (unsigned long)strtoul(v, (char **)NULL, 0);
          else {
            n->string = malloc(strlen(v+1) + 1);
            /* strip quotation marks */
            v[strlen(v)-1] = '\0';
            strcpy(n->string, v+1);
          }
        }
        n->type = malloc(strlen(tmp) + 1);
        strcpy(n->type, tmp);
      }
    }
    /* parameters, separated by white space */
    else {
      tmp[c++] = *s;
    }
  }

  return first;
} /* parse */


/*
* Parse parameter=value. Return value, word becomes parameter.
* Value must be unsigned integer.
*/
static uint32_t parse_uint(char *word)
{
  char *s;
  uint32_t value = 0;

  if ((s = strchr(word, '='))) {
    value = strtoul(s+1, (char **)NULL, 0);
    *s = '\0';
  }
  else {
    *word = '\0';
  }
  return value;
} /* parse_int */


/*
* Free nodes recursively.
*/
static void node_free(node_t *list)
{
  node_t *n, *n_next;

  for (n = list; n; n = n_next) {
    if (n->type) {
      free(n->type);
      n->type = 0;
    }
    if (n->string) {
      free(n->string);
      n->string = 0;
    }
    n->num = 0;
    if (n->list) node_free(n->list);
    n_next = n->next;
    free(n);
  }
} /* node_free */


static int rtcp_sdes_item(char *type, char *string, char *packet)
{
  struct {
    const char *name;
    rtcp_sdes_type_t type;
  } map[] = {
    {"end",   RTCP_SDES_END},
    {"cname", RTCP_SDES_CNAME},
    {"name",  RTCP_SDES_NAME},
    {"email", RTCP_SDES_EMAIL},
    {"phone", RTCP_SDES_PHONE},
    {"loc",   RTCP_SDES_LOC},
    {"tool",  RTCP_SDES_TOOL},
    {"note",  RTCP_SDES_NOTE},
    {"priv",  RTCP_SDES_PRIV},
    {0,0}
  };
  int i;
  rtcp_sdes_item_t *item = (rtcp_sdes_item_t *)packet;

  for (i = 0; map[i].name; i++) {
    if (strcasecmp(type, map[i].name) == 0) break;
  }

  item->type = map[i].type;
  item->length = strlen(string);
  strcpy(item->data, string);

  return item->length + 2;
} /* rtcp_sdes_item */


/*
* Create SDES entries for single source.
* Return length.
*/
static int rtcp_sdes(node_t *list, char *packet)
{
  node_t *n;
  int len = 0, total = 4;
  struct rtcp_sdes *sdes = (struct rtcp_sdes *)packet;

  packet += 4; /* skip SRC */
  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "src") == 0) {
        sdes->src = htonl(n->num);
      }
      else {
        len = rtcp_sdes_item(n->type, n->string, packet);
        packet += len;
        total += len;
      }
    }
  }
  /* end marker */
  *packet = RTCP_SDES_END;
  total++;

  /* pad length to next multiple of 32 bits */
  len = (total + 3) & 0xfffc;
  memset(packet, 0, len - total);

  return len;
} /* rtcp_sdes */


#define RTCP_SDES_HDR_LEN  4  /* SDES default length (common) */

static int rtcp_write_sdes(node_t *list, char *packet)
{
  node_t *n;
  int len = 0, total = RTCP_SDES_HDR_LEN, count = 0;
  rtcp_t *r = (rtcp_t *)packet;

  r->common.length  = 0;
  r->common.count   = 0;
  r->common.version = RTP_VERSION;
  r->common.pt      = RTCP_SDES;
  r->common.p       = 0;

  packet += RTCP_SDES_HDR_LEN; /* skip common header */

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "SDES") == 0)
        continue;
      else if (strcmp(n->type, "p") == 0)
        r->common.p = n->num;
      else if (strcmp(n->type, "count") == 0)
        r->common.count = n->num;
      else if (strcmp(n->type, "len") == 0)
        r->common.length = htons(n->num);
      else  {
        fprintf(stderr, "Invalid RTCP type %s\n", n->type);
        exit(2);
      }
    }
    else { /* list: type-specific parts */
      len = rtcp_sdes(n->list, packet);
      packet += len;
      total += len;
      count++;
    }
  }
  /* if no length or count given, fill in */
  if (r->common.length == 0) {
    r->common.length = htons((total - 4) / 4);
  }
  if (r->common.count == 0)
    r->common.count = count;

  return total;
} /* rtcp_write_sdes */



/*
* Create RR entries for single report block.
* Return length.
*/
static int rtcp_rr(node_t *list, char *packet)
{
  node_t *n;
  rtcp_rr_t *rr = (rtcp_rr_t *)packet;

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "ssrc") == 0)
        rr->ssrc = htonl(n->num);
      else if (strcmp(n->type, "fraction") == 0)
        rr->fraction = (n->num)*256;
      else if (strcmp(n->type, "lost") == 0)   /* PP: alignment OK? */
        RTCP_SET_LOST(rr, n->num);
      else if (strcmp(n->type, "last_seq") == 0)
        rr->last_seq = htonl(n->num);
      else if (strcmp(n->type, "jit") == 0)
        rr->jitter = htonl(n->num);
      else if (strcmp(n->type, "lsr") == 0)
        rr->lsr = htonl(n->num);
      else if (strcmp(n->type, "dlsr") == 0)
        rr->dlsr = htonl(n->num);
      else  {
        fprintf(stderr, "Invalid RTCP RR type %s\n", n->type);
        exit(2);
      }
    }
  }

  return  sizeof(rtcp_rr_t);
} /* rtcp_rr */


#define RTCP_SR_HDR_LEN  28  /* SR default length (common + ssrc... ) */

/*
 * Number of seconds between 1-Jan-1900 and 1-Jan-1970
 */
#define GETTIMEOFDAY_TO_NTP_OFFSET 2208988800UL


/*
 * convert microseconds to fraction of second * 2^32 (i.e., the lsw of
 * a 64-bit ntp timestamp).  This routine uses the factorization
 * 2^32/10^6 = 4096 + 256 - 1825/32 which results in a max conversion
 * error of 3 * 10^-7 and an average error of half that.
 */
static u_int usec2ntp(u_int usec)
{
  u_int t = (usec * 1825) >> 5;
  return ((usec << 12) + (usec << 8) - t);
} /* usec2ntp */


static int rtcp_write_sr(node_t *list, char *packet)
{
  node_t *n;
  int len = 0, total = RTCP_SR_HDR_LEN, count = 0;
  rtcp_t *r = (rtcp_t *)packet;
  struct timeval now;

  gettimeofday(&now, 0);
  r->common.length  = 0;
  r->common.count   = 0;
  r->common.version = RTP_VERSION;
  r->common.pt      = RTCP_SR;
  r->common.p       = 0;
  r->r.sr.ntp_sec   = htonl((uint32_t)now.tv_sec + GETTIMEOFDAY_TO_NTP_OFFSET);
  r->r.sr.ntp_frac  = htonl(usec2ntp((u_int)now.tv_usec));

  packet += RTCP_SR_HDR_LEN; /* skip common header and ssrc */

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "SR") == 0)
        continue;
      else if (strcmp(n->type, "ssrc") == 0)
        r->r.sr.ssrc = htonl(n->num);
      else if (strcmp(n->type, "p") == 0)
        r->common.p = n->num;
      else if (strcmp(n->type, "count") == 0)
        r->common.count = n->num;
      else if (strcmp(n->type, "len") == 0)
        r->common.length = htons(n->num);
      else if (strcmp(n->type, "ntp") == 0)  /* PP: two words */
        r->r.sr.ntp_sec = htonl(n->num);
      else if (strcmp(n->type, "ts") == 0)
        r->r.sr.rtp_ts = htonl(n->num);
      else if (strcmp(n->type, "psent") == 0)
        r->r.sr.psent = htonl(n->num);
      else if (strcmp(n->type, "osent") == 0)
        r->r.sr.osent = htonl(n->num);
      else  {
        fprintf(stderr, "Invalid RTCP type %s\n", n->type);
        exit(2);
      }
    }
    else { /* list: type-specific parts */
      len = rtcp_rr(n->list, packet);
      packet += len;
      total += len;
      count++;
    }
  }
  /* if no length or count given, fill in */
  if (r->common.length == 0) {
    r->common.length = htons((total - 4) / 4);
  }
  if (r->common.count == 0)
    r->common.count = count;

  return total;
} /* rtcp_write_sr */



#define RTCP_RR_HDR_LEN  8  /* RR default length (common + ssrc) */

static int rtcp_write_rr(node_t *list, char *packet)
{
  node_t *n;
  int len = 0, total = RTCP_RR_HDR_LEN, count = 0;
  rtcp_t *r = (rtcp_t *)packet;

  r->common.length  = 0;
  r->common.count   = 0;
  r->common.version = RTP_VERSION;
  r->common.pt      = RTCP_RR;
  r->common.p       = 0;

  packet += RTCP_RR_HDR_LEN; /* skip common header and ssrc */

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "RR") == 0)
        continue;
      else if (strcmp(n->type, "ssrc") == 0)
        r->r.rr.ssrc = htonl(n->num);
      else if (strcmp(n->type, "p") == 0)
        r->common.p = n->num;
      else if (strcmp(n->type, "count") == 0)
        r->common.count = n->num;
      else if (strcmp(n->type, "len") == 0)
        r->common.length = htons(n->num);
      else  {
        fprintf(stderr, "Invalid RTCP type %s\n", n->type);
        exit(2);
      }
    }
    else { /* list: type-specific parts */
      len = rtcp_rr(n->list, packet);
      packet += len;
      total += len;
      count++;
    }
  }
  /* if no length or count given, fill in */
  if (r->common.length == 0) {
    r->common.length = htons((total - 4) / 4);
  }
  if (r->common.count == 0)
    r->common.count = count;

  return total;
} /* rtcp_write_rr */


static int rtcp_bye(node_t *list, char *packet)
{
  node_t *n;
  uint32_t *bye = (uint32_t *)packet;

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "ssrc") == 0)
        *bye = htonl(n->num);
    }
  }
  return sizeof(uint32_t);
} /* rtcp_bye */


#define RTCP_BYE_HDR_LEN  4  /* BYE default length (common) */

static int rtcp_write_bye(node_t *list, char *packet)
{
  node_t *n;
  int len = 0, total = RTCP_BYE_HDR_LEN, count = 0;
  rtcp_t *r = (rtcp_t *)packet;

  r->common.length  = 0;
  r->common.count   = 0;
  r->common.version = RTP_VERSION;
  r->common.pt      = RTCP_BYE;
  r->common.p       = 0;

  packet += RTCP_BYE_HDR_LEN; /* skip common header */

  for (n = list; n; n = n->next) {
    if (n->type) {
      if (strcmp(n->type, "BYE") == 0)
        continue;
      else if (strcmp(n->type, "p") == 0)
        r->common.p = n->num;
      else if (strcmp(n->type, "count") == 0)
        r->common.count = n->num;
      else if (strcmp(n->type, "len") == 0)
        r->common.length = htons(n->num);
      else  {
        fprintf(stderr, "Invalid RTCP type %s\n", n->type);
        exit(2);
      }
    }
    else { /* list: type-specific parts */
      len = rtcp_bye(n->list, packet);
      packet += len;
      total += len;
      count++;
    }
  }
  /* if no length or count given, fill in */
  if (r->common.length == 0) {
    r->common.length = htons((total - 4) / 4);
  }
  if (r->common.count == 0)
    r->common.count = count;

  return total;
} /* rtcp_write_bye */


static int rtcp_write_app(node_t *list, char *packet)
{
  return 0;
}

/*
 * Based on list of parameters in 'n', assemble RTCP packet.
 */
static int rtcp_packet(node_t *list, char *packet)
{
  struct {
    const char *pt;
    int  (*rtcp_write)(node_t *list, char *packet);
  } rtcp_map[] = {
    { "SDES",  rtcp_write_sdes },
    { "RR",  rtcp_write_rr },
    { "SR",  rtcp_write_sr },
    { "BYE",  rtcp_write_bye },
    { "APP",  rtcp_write_app },
  };
  int max = sizeof(rtcp_map) / sizeof(rtcp_map[0]);
  int i;
  node_t *n;

  for (n = list; n; n = n->next) {
    if (!n->type)
      continue;

    for (i=0; i < max; i++) {
      if (strcmp(n->type, rtcp_map[i].pt) == 0)
        return rtcp_map[i].rtcp_write(list, packet);
    }
  }

  fprintf(stderr, "No RTCP payload type\n");
  exit(2);
} /* rtcp_packet */


/*
* Generate RTCP packet based on textual description.
*/
static int rtcp(char *text, char *packet)
{
  node_t *node_list, *n;
  int len;
  int total = 0;

  node_list = parse(text);

  for (n = node_list; n; n = n->next) {
    if (n->list) {
      len = rtcp_packet(n->list, packet);
      packet += len;
      total += len;
    }
  }
  node_free(node_list);

  return total;
} /* rtcp */


/*
* Generate RTP data packet.
*/
static int rtp(char *text, char *packet)
{
  char *word;
  int pl = 0;  /* payload length */
  int ext_pl = 0;  /* extension payload length */
  int wc = 0;
  int cc = 0;
  rtp_hdr_t *h = (rtp_hdr_t *)packet;
  rtp_hdr_ext_t *ext;
  int length = 0;
  uint32_t value;

  /* defaults */
  memset(packet, 0, sizeof(rtp_hdr_t));
  h->version = RTP_VERSION;

  while ((word = strtok(wc ? 0 : text, " "))) {
    value = parse_uint(word);
    if (strcmp(word, "ts") == 0) {
      h->ts = htonl(value);
    }
    else if (strcmp(word, "seq") == 0) {
      h->seq = htons(value);
    }
    else if (strcmp(word, "pt") == 0) {
      h->pt = value;
    }
    else if (strcmp(word, "ssrc") == 0) {
      h->ssrc = htonl(value);
    }
    else if (strcmp(word, "p") == 0) {
      h->p = (value != 0);
    }
    else if (strcmp(word, "m") == 0) {
      h->m = value;
    }
    else if (strcmp(word, "x") == 0) {
      h->x = value;
    }
    else if (strcmp(word, "v") == 0) {
      h->version = value;
    }
    else if (strcmp(word, "cc") == 0) {
      h->cc = value;
    }
    else if (strncmp(word, "csrc", 4) == 0) {
      int k = atoi(&word[5]);
      h->csrc[k] = value;
      if (k > cc) cc = k;
    }
    /* we'd better have h->cc already */
    else if (strcmp(word, "ext_type") == 0) {
      ext = (rtp_hdr_ext_t *)(packet + 12 + h->cc*4);
      ext->ext_type = htons(value);
      ext_pl += sizeof(rtp_hdr_ext_t);
    }
    else if (strcmp(word, "ext_len") == 0) {
      ext = (rtp_hdr_ext_t *)(packet + 12 + h->cc*4);
      ext_pl += value * 4;
      ext->len = htons(value);
    }
    /* we'd better have a valid ext_pl already */
    else if (strcmp(word, "ext_data") == 0) {
      int dummy;
      dummy = hex(&word[9], packet + 12 + h->cc*4 + 4);
    }
    /* data is in hex; words may be separated by spaces */
    else if (strcmp(word, "data") == 0) {
      pl = hex(&word[5], packet + 12 + h->cc*4 + ext_pl);
    }
    else if (strcmp(word, "len") == 0) {
      length = value;
    }
    wc++;
  }
  /* fill in default values if not set */
  if (h->cc == 0) h->cc = cc;
  if (length == 0) length = 12 + h->cc * 4 + pl + ext_pl;

  /* insert padding */

  return length;
} /* rtp */


/*
* Generate a packet based on description in 'text'. Return length.
* Set generation time.
*/
static int generate(char *text, char *data, struct timeval *time, int *type)
{
  int length;
  char type_name[100];
  /* suseconds_t is int on some platforms, long on others, so it can't portably
     be directly used in sscanf.  sscanf into a long and assign (which implicitly
     casts) */
  long tv_usec;

  if (verbose) printf("%s", text);
  if (sscanf(text, "%ld.%ld %s", &(time->tv_sec), &tv_usec, type_name) < 3) {
    fprintf(stderr, "Line {%s} is invalid.\n", text);
    exit(2);
  }
  time->tv_usec = tv_usec;
  if (strcmp(type_name, "RTP") == 0) {
    length = rtp(strstr(text, "RTP") + 3, data);
    *type = 0;
  }
  else if (strcmp(type_name, "RTCP") == 0) {
    length = rtcp(strstr(text, "RTCP") + 4, data);
    *type = 1;
  } else {
    fprintf(stderr, "Type %s is not supported.\n", type_name);
    exit(2);
  }
  return length;
} /* generate */

#define MAX_TEXT_LINE 4096

/*
* Timer handler; sends any pending packets and parses next one.
* First packet is played out immediately.
*/
static Notify_value send_handler(Notify_client client)
{
  static struct {
    int length;
    struct timeval time;
    int type;
    char data[1500];
  } packet;
  FILE *in = (FILE *)client;
  static char line[MAX_TEXT_LINE];       /* last line read (may be next packet) */
  char text[MAX_TEXT_LINE];              /* current line from the file, including cont. lines */
  static int isfirstpacket = 1; /* is this the first packet? */
  struct timeval this_tv;       /* time this packet is being sent */
  static struct timeval basetime;        /* base time (first packet) */
  struct timeval next_tv;       /* time for next packet */
  struct timeval past_tv;       /* to determine the time to sent is in past */
  char *s;

  gettimeofday(&this_tv, NULL);

  /* send any pending packet */
  if (packet.length && send(sock[packet.type], packet.data, packet.length, 0) < 0) {
    perror("write");
  }

  /* read line; continuation lines start with white space */
  if (feof(in)) {
    if (loop) {
      fseek(in, 0, SEEK_SET);
      printf("Rewound input file\n");
    }
    else {
      notify_stop();
      exit(0);
      return NOTIFY_DONE;
   }
  }
  s = text;
  if (line[0]) {
    strcpy(text, line);
    s += strlen(text);
  }
  while (fgets(line, sizeof(line), in)) {
    if (line[0] == '#') continue;
    else if (s != text && !isspace((int)line[0])) break;
    else {
      strcpy(s, line);
      s += strlen(line);
    }
  }

  packet.length = generate(text, packet.data, &packet.time, &packet.type);
  /* very first packet: send immediately */
  if (isfirstpacket) {
    isfirstpacket = 0;
    timersub(&this_tv, &packet.time, &basetime);
  }

  /* compute and set next playout time */
  timeradd(&basetime, &packet.time, &next_tv);

  timersub(&next_tv, &this_tv, &past_tv);
  if (past_tv.tv_sec < 0) {
    fprintf(stderr, "Non-monotonic time %ld.%ld - sent immediately.\n", 
            packet.time.tv_sec, (long)packet.time.tv_usec);
    next_tv = this_tv;
  }

  timer_set(&next_tv, send_handler, (Notify_client)in, 0);
  return NOTIFY_DONE;
} /* send_handler */


int main(int argc, char *argv[])
{
  unsigned char ttl = 16;
  static struct sockaddr_in sin;
  static struct sockaddr_in from;
  int i;
  int c;
  int alert = 0;       /* insert IP router alert option if possible */
  int sourceport = 0;  /* source port */
  int on = 1;          /* flag */
  static u_char ra[4] = {148, 4, 0, 1};  /* router alert option for RTP */
  char *filename = 0;
  extern char *optarg;
  extern int optind;

  /* parse command line arguments */
  startupSocket();
  while ((c = getopt(argc, argv, "f:als:v?h")) != EOF) {
    switch(c) {
    case 'f':
      filename = optarg;
      break;
    case 'a':
      alert = 1;
      break;
    case 'l':  /* loop */
      loop = 1;
      break;
    case 's':  /* locked source port */
      sourceport = atoi(optarg);
      break;
    case 'v':
      verbose = 1;
      break;
    case '?':
    case 'h':
      usage(argv[0]);
      break;
    }
  }

  if (filename) {
    in = fopen(filename, "r");
    if (!in) {
      perror(filename);
      exit(1);
    }
  }
  else {
    in = stdin;
    loop = 0;
  }

  if (optind < argc) {
    if (hpt(argv[optind], &sin, &ttl) == -1) {
      fprintf(stderr, "%s: Invalid host. %s\n", argv[0], argv[optind]);
      usage(argv[0]);
      exit(1);
    }
    if (sin.sin_addr.s_addr == INADDR_ANY) {
      struct hostent *host;
      struct in_addr *local;
      if ((host = gethostbyname("localhost")) == NULL) {
        perror("gethostbyname()");
        exit(1);
      }
      local = (struct in_addr *)host->h_addr_list[0];
      sin.sin_addr = *local;
    }
  }

  /* create/connect sockets */
  for (i = 0; i < 2; i++) {
    sock[i] = socket(PF_INET, SOCK_DGRAM, 0);
    if (sock[i] < 0) {
      perror("socket");
      exit(1);
    }
    sin.sin_port = htons(ntohs(sin.sin_port) + i);

    if (sourceport) {
      memset((char *)(&from), 0, sizeof(struct sockaddr_in));
      from.sin_family      = PF_INET;
      from.sin_addr.s_addr = INADDR_ANY;
      from.sin_port        = htons(sourceport + i);

      if (setsockopt(sock[i], SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("SO_REUSEADDR");
        exit(1);
      }

#ifdef SO_REUSEPORT
      if (setsockopt(sock[i], SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        perror("SO_REUSEPORT");
        exit(1);
      }
#endif

      if (bind(sock[i], (struct sockaddr *)&from, sizeof(from)) < 0) {
        perror("bind");
        exit(1);
      }
    }

    if (connect(sock[i], (struct sockaddr *)&sin, sizeof(sin)) < 0) {
      perror("connect");
      exit(1);
    }

    if (IN_CLASSD(sin.sin_addr.s_addr) &&
        (setsockopt(sock[i], IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
                   sizeof(ttl)) < 0)) {
      perror("IP_MULTICAST_TTL");
      exit(1);
    }
    if (alert &&
        (setsockopt(sock[i], IPPROTO_IP, IP_OPTIONS, (void *)ra,
                  sizeof(ra)) < 0)) {
      perror("IP router alert option");
      exit(1);
    }
  }

  send_handler((Notify_client)in);
  notify_start();
  return 0;
} /* main */
