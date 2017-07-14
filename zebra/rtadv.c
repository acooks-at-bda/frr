/* Router advertisement
 * Copyright (C) 2016 Cumulus Networks
 * Copyright (C) 2005 6WIND <jean-mickael.guerin@6wind.com>
 * Copyright (C) 1999 Kunihiro Ishiguro
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
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "memory.h"
#include "zebra_memory.h"
#include "sockopt.h"
#include "thread.h"
#include "if.h"
#include "stream.h"
#include "log.h"
#include "prefix.h"
#include "linklist.h"
#include "command.h"
#include "privs.h"
#include "vrf.h"

#include "zebra/interface.h"
#include "zebra/rtadv.h"
#include "zebra/debug.h"
#include "zebra/rib.h"
#include "zebra/zserv.h"
#include "zebra/zebra_ns.h"
#include "zebra/zebra_vrf.h"

extern struct zebra_privs_t zserv_privs;

#if defined (HAVE_RTADV)

#ifdef OPEN_BSD
#include <netinet/icmp6.h>
#endif

/* If RFC2133 definition is used. */
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP 
#endif
#ifndef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP 
#endif

#define ALLNODE   "ff02::1"
#define ALLROUTER "ff02::2"

enum rtadv_event {RTADV_START, RTADV_STOP, RTADV_TIMER, 
		  RTADV_TIMER_MSEC, RTADV_READ};

static void rtadv_event (struct zebra_ns *, enum rtadv_event, int);

static int if_join_all_router (int, struct interface *);
static int if_leave_all_router (int, struct interface *);

static int
rtadv_increment_received(struct zebra_ns *zns, ifindex_t *ifindex)
{
  int ret = -1;
  struct interface *iface;
  struct zebra_if *zif;

  iface = if_lookup_by_index_per_ns (zns, *ifindex);
  if (iface && iface->info)
    {
      zif = iface->info;
      zif->ra_rcvd++;
      ret = 0;
    }
  return ret;
}

static int
rtadv_recv_packet (struct zebra_ns *zns, int sock, u_char *buf, int buflen,
		   struct sockaddr_in6 *from, ifindex_t *ifindex,
		   int *hoplimit)
{
  int ret;
  struct msghdr msg;
  struct iovec iov;
  struct cmsghdr  *cmsgptr;
  struct in6_addr dst;

  char adata[1024];

  /* Fill in message and iovec. */
  msg.msg_name = (void *) from;
  msg.msg_namelen = sizeof (struct sockaddr_in6);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (void *) adata;
  msg.msg_controllen = sizeof adata;
  iov.iov_base = buf;
  iov.iov_len = buflen;

  /* If recvmsg fail return minus value. */
  ret = recvmsg (sock, &msg, 0);
  if (ret < 0)
    return ret;

  for (cmsgptr = ZCMSG_FIRSTHDR(&msg); cmsgptr != NULL;
       cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) 
    {
      /* I want interface index which this packet comes from. */
      if (cmsgptr->cmsg_level == IPPROTO_IPV6 &&
	  cmsgptr->cmsg_type == IPV6_PKTINFO) 
	{
	  struct in6_pktinfo *ptr;
	  
	  ptr = (struct in6_pktinfo *) CMSG_DATA (cmsgptr);
	  *ifindex = ptr->ipi6_ifindex;
	  memcpy(&dst, &ptr->ipi6_addr, sizeof(ptr->ipi6_addr));
        }

      /* Incoming packet's hop limit. */
      if (cmsgptr->cmsg_level == IPPROTO_IPV6 &&
	  cmsgptr->cmsg_type == IPV6_HOPLIMIT)
	{
	  int *hoptr = (int *) CMSG_DATA (cmsgptr);
	  *hoplimit = *hoptr;
	}
    }

  rtadv_increment_received(zns, ifindex);
  return ret;
}

#define RTADV_MSG_SIZE 4096

/* Send router advertisement packet. */
static void
rtadv_send_packet (int sock, struct interface *ifp)
{
  struct msghdr msg;
  struct iovec iov;
  struct cmsghdr  *cmsgptr;
  struct in6_pktinfo *pkt;
  struct sockaddr_in6 addr;
  static void *adata = NULL;
  unsigned char buf[RTADV_MSG_SIZE];
  struct nd_router_advert *rtadv;
  int ret;
  int len = 0;
  struct zebra_if *zif;
  struct rtadv_prefix *rprefix;
  u_char all_nodes_addr[] = {0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
  struct listnode *node;
  u_int16_t pkt_RouterLifetime;

  /*
   * Allocate control message bufffer.  This is dynamic because
   * CMSG_SPACE is not guaranteed not to call a function.  Note that
   * the size will be different on different architectures due to
   * differing alignment rules.
   */
  if (adata == NULL)
    {
      /* XXX Free on shutdown. */
      adata = malloc(CMSG_SPACE(sizeof(struct in6_pktinfo)));
	   
      if (adata == NULL)
	zlog_err("rtadv_send_packet: can't malloc control data");
    }

  /* Logging of packet. */
  if (IS_ZEBRA_DEBUG_PACKET)
    zlog_debug ("%s(%u): Tx RA, socket %u",
                ifp->name, ifp->ifindex, sock);

  /* Fill in sockaddr_in6. */
  memset (&addr, 0, sizeof (struct sockaddr_in6));
  addr.sin6_family = AF_INET6;
#ifdef SIN6_LEN
  addr.sin6_len = sizeof (struct sockaddr_in6);
#endif /* SIN6_LEN */
  addr.sin6_port = htons (IPPROTO_ICMPV6);
  IPV6_ADDR_COPY (&addr.sin6_addr, all_nodes_addr);

  /* Fetch interface information. */
  zif = ifp->info;

  /* Make router advertisement message. */
  rtadv = (struct nd_router_advert *) buf;

  rtadv->nd_ra_type = ND_ROUTER_ADVERT;
  rtadv->nd_ra_code = 0;
  rtadv->nd_ra_cksum = 0;

  rtadv->nd_ra_curhoplimit = 64;

  /* RFC4191: Default Router Preference is 0 if Router Lifetime is 0. */
  rtadv->nd_ra_flags_reserved =
    zif->rtadv.AdvDefaultLifetime == 0 ? 0 : zif->rtadv.DefaultPreference;
  rtadv->nd_ra_flags_reserved <<= 3;

  if (zif->rtadv.AdvManagedFlag)
    rtadv->nd_ra_flags_reserved |= ND_RA_FLAG_MANAGED;
  if (zif->rtadv.AdvOtherConfigFlag)
    rtadv->nd_ra_flags_reserved |= ND_RA_FLAG_OTHER;
  if (zif->rtadv.AdvHomeAgentFlag)
    rtadv->nd_ra_flags_reserved |= ND_RA_FLAG_HOME_AGENT;
  /* Note that according to Neighbor Discovery (RFC 4861 [18]),
   * AdvDefaultLifetime is by default based on the value of
   * MaxRtrAdvInterval.  AdvDefaultLifetime is used in the Router Lifetime
   * field of Router Advertisements.  Given that this field is expressed
   * in seconds, a small MaxRtrAdvInterval value can result in a zero
   * value for this field.  To prevent this, routers SHOULD keep
   * AdvDefaultLifetime in at least one second, even if the use of
   * MaxRtrAdvInterval would result in a smaller value. -- RFC6275, 7.5 */
  pkt_RouterLifetime = zif->rtadv.AdvDefaultLifetime != -1 ?
    zif->rtadv.AdvDefaultLifetime :
    MAX (1, 0.003 * zif->rtadv.MaxRtrAdvInterval);
  rtadv->nd_ra_router_lifetime = htons (pkt_RouterLifetime);
  rtadv->nd_ra_reachable = htonl (zif->rtadv.AdvReachableTime);
  rtadv->nd_ra_retransmit = htonl (0);

  len = sizeof (struct nd_router_advert);

  /* If both the Home Agent Preference and Home Agent Lifetime are set to
   * their default values specified above, this option SHOULD NOT be
   * included in the Router Advertisement messages sent by this home
   * agent. -- RFC6275, 7.4 */
  if
  (
    zif->rtadv.AdvHomeAgentFlag &&
    (zif->rtadv.HomeAgentPreference || zif->rtadv.HomeAgentLifetime != -1)
  )
    {
      struct nd_opt_homeagent_info *ndopt_hai = 
	(struct nd_opt_homeagent_info *)(buf + len);
      ndopt_hai->nd_opt_hai_type = ND_OPT_HA_INFORMATION;
      ndopt_hai->nd_opt_hai_len = 1;
      ndopt_hai->nd_opt_hai_reserved = 0;
      ndopt_hai->nd_opt_hai_preference = htons(zif->rtadv.HomeAgentPreference);
      /* 16-bit unsigned integer.  The lifetime associated with the home
       * agent in units of seconds.  The default value is the same as the
       * Router Lifetime, as specified in the main body of the Router
       * Advertisement.  The maximum value corresponds to 18.2 hours.  A
       * value of 0 MUST NOT be used. -- RFC6275, 7.5 */
      ndopt_hai->nd_opt_hai_lifetime = htons
      (
        zif->rtadv.HomeAgentLifetime != -1 ?
        zif->rtadv.HomeAgentLifetime :
        MAX (1, pkt_RouterLifetime) /* 0 is OK for RL, but not for HAL*/
      );
      len += sizeof(struct nd_opt_homeagent_info);
    }

  if (zif->rtadv.AdvIntervalOption)
    {
      struct nd_opt_adv_interval *ndopt_adv = 
	(struct nd_opt_adv_interval *)(buf + len);
      ndopt_adv->nd_opt_ai_type = ND_OPT_ADV_INTERVAL;
      ndopt_adv->nd_opt_ai_len = 1;
      ndopt_adv->nd_opt_ai_reserved = 0;
      ndopt_adv->nd_opt_ai_interval = htonl(zif->rtadv.MaxRtrAdvInterval);
      len += sizeof(struct nd_opt_adv_interval);
    }

  /* Fill in prefix. */
  for (ALL_LIST_ELEMENTS_RO (zif->rtadv.AdvPrefixList, node, rprefix))
    {
      struct nd_opt_prefix_info *pinfo;

      pinfo = (struct nd_opt_prefix_info *) (buf + len);

      pinfo->nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION;
      pinfo->nd_opt_pi_len = 4;
      pinfo->nd_opt_pi_prefix_len = rprefix->prefix.prefixlen;

      pinfo->nd_opt_pi_flags_reserved = 0;
      if (rprefix->AdvOnLinkFlag)
	pinfo->nd_opt_pi_flags_reserved |= ND_OPT_PI_FLAG_ONLINK;
      if (rprefix->AdvAutonomousFlag)
	pinfo->nd_opt_pi_flags_reserved |= ND_OPT_PI_FLAG_AUTO;
      if (rprefix->AdvRouterAddressFlag)
	pinfo->nd_opt_pi_flags_reserved |= ND_OPT_PI_FLAG_RADDR;

      pinfo->nd_opt_pi_valid_time = htonl (rprefix->AdvValidLifetime);
      pinfo->nd_opt_pi_preferred_time = htonl (rprefix->AdvPreferredLifetime);
      pinfo->nd_opt_pi_reserved2 = 0;

      IPV6_ADDR_COPY (&pinfo->nd_opt_pi_prefix, &rprefix->prefix.prefix);

#ifdef DEBUG
      {
	u_char buf[INET6_ADDRSTRLEN];

	zlog_debug ("DEBUG %s", inet_ntop (AF_INET6, &pinfo->nd_opt_pi_prefix, 
	           buf, INET6_ADDRSTRLEN));

      }
#endif /* DEBUG */

      len += sizeof (struct nd_opt_prefix_info);
    }

  /* Hardware address. */
  if (ifp->hw_addr_len != 0)
    {
      buf[len++] = ND_OPT_SOURCE_LINKADDR;

      /* Option length should be rounded up to next octet if
         the link address does not end on an octet boundary. */
      buf[len++] = (ifp->hw_addr_len + 9) >> 3;

      memcpy (buf + len, ifp->hw_addr, ifp->hw_addr_len);
      len += ifp->hw_addr_len;

      /* Pad option to end on an octet boundary. */
      memset (buf + len, 0, -(ifp->hw_addr_len + 2) & 0x7);
      len += -(ifp->hw_addr_len + 2) & 0x7;
    }

  /* MTU */
  if (zif->rtadv.AdvLinkMTU)
    {
      struct nd_opt_mtu * opt = (struct nd_opt_mtu *) (buf + len);
      opt->nd_opt_mtu_type = ND_OPT_MTU;
      opt->nd_opt_mtu_len = 1;
      opt->nd_opt_mtu_reserved = 0;
      opt->nd_opt_mtu_mtu = htonl (zif->rtadv.AdvLinkMTU);
      len += sizeof (struct nd_opt_mtu);
    }

  msg.msg_name = (void *) &addr;
  msg.msg_namelen = sizeof (struct sockaddr_in6);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = (void *) adata;
  msg.msg_controllen = CMSG_SPACE(sizeof(struct in6_pktinfo));
  msg.msg_flags = 0;
  iov.iov_base = buf;
  iov.iov_len = len;

  cmsgptr = ZCMSG_FIRSTHDR(&msg);
  cmsgptr->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
  cmsgptr->cmsg_level = IPPROTO_IPV6;
  cmsgptr->cmsg_type = IPV6_PKTINFO;

  pkt = (struct in6_pktinfo *) CMSG_DATA (cmsgptr);
  memset (&pkt->ipi6_addr, 0, sizeof (struct in6_addr));
  pkt->ipi6_ifindex = ifp->ifindex;

  ret = sendmsg (sock, &msg, 0);
  if (ret < 0)
    {
      zlog_err ("%s(%u): Tx RA failed, socket %u error %d (%s)",
                ifp->name, ifp->ifindex, sock, errno, safe_strerror(errno));
    }
  else
    zif->ra_sent++;
}

static int
rtadv_timer (struct thread *thread)
{
  struct zebra_ns *zns = THREAD_ARG (thread);
  struct vrf *vrf;
  struct listnode *node, *nnode;
  struct interface *ifp;
  struct zebra_if *zif;
  int period;

  zns->rtadv.ra_timer = NULL;
  if (zns->rtadv.adv_msec_if_count == 0)
    {
      period = 1000; /* 1 s */
      rtadv_event (zns, RTADV_TIMER, 1 /* 1 s */);
    } 
  else
    {
      period = 10; /* 10 ms */
      rtadv_event (zns, RTADV_TIMER_MSEC, 10 /* 10 ms */);
    }

  RB_FOREACH (vrf, vrf_id_head, &vrfs_by_id)
    for (ALL_LIST_ELEMENTS (vrf->iflist, node, nnode, ifp))
      {
        if (if_is_loopback (ifp) ||
            CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK) ||
            ! if_is_operative (ifp))
          continue;

        zif = ifp->info;

        if (zif->rtadv.AdvSendAdvertisements)
          {
            if (zif->rtadv.inFastRexmit)
              {
                /* We assume we fast rexmit every sec so no additional vars */
                if (--zif->rtadv.NumFastReXmitsRemain <= 0)
                  zif->rtadv.inFastRexmit = 0;

                if (IS_ZEBRA_DEBUG_SEND)
                  zlog_debug("Fast RA Rexmit on interface %s", ifp->name);

                rtadv_send_packet (zns->rtadv.sock, ifp);
              }
            else
              {
                zif->rtadv.AdvIntervalTimer -= period;
                if (zif->rtadv.AdvIntervalTimer <= 0)
                  {
                    /* FIXME: using MaxRtrAdvInterval each time isn't what section
                       6.2.4 of RFC4861 tells to do. */
                    zif->rtadv.AdvIntervalTimer = zif->rtadv.MaxRtrAdvInterval;
                    rtadv_send_packet (zns->rtadv.sock, ifp);
                  }
              }
          }
      }

  return 0;
}

static void
rtadv_process_solicit (struct interface *ifp)
{
  struct zebra_vrf *zvrf = vrf_info_lookup (ifp->vrf_id);
  struct zebra_ns *zns = zvrf->zns;

  assert (zns);
  rtadv_send_packet (zns->rtadv.sock, ifp);
}

static void
rtadv_process_advert (u_char *msg, unsigned int len, struct interface *ifp,
                      struct sockaddr_in6 *addr)
{
  struct nd_router_advert *radvert;
  char addr_str[INET6_ADDRSTRLEN];
  struct zebra_if *zif;
  struct prefix p;

  zif = ifp->info;

  inet_ntop (AF_INET6, &addr->sin6_addr, addr_str, INET6_ADDRSTRLEN);

  if (len < sizeof(struct nd_router_advert)) {
      zlog_warn("%s(%u): Rx RA with invalid length %d from %s",
                ifp->name, ifp->ifindex, len, addr_str);
      return;
  }
  if (!IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) {
      zlog_warn("%s(%u): Rx RA with non-linklocal source address from %s",
                ifp->name, ifp->ifindex, addr_str);
      return;
  }

  radvert = (struct nd_router_advert *) msg;

  if ((radvert->nd_ra_curhoplimit && zif->rtadv.AdvCurHopLimit) &&
      (radvert->nd_ra_curhoplimit != zif->rtadv.AdvCurHopLimit))
    {
      zlog_warn("%s(%u): Rx RA - our AdvCurHopLimit doesn't agree with %s",
                ifp->name, ifp->ifindex, addr_str);
    }

  if ((radvert->nd_ra_flags_reserved & ND_RA_FLAG_MANAGED) &&
      !zif->rtadv.AdvManagedFlag)
    {
      zlog_warn("%s(%u): Rx RA - our AdvManagedFlag doesn't agree with %s",
                ifp->name, ifp->ifindex, addr_str);
    }

  if ((radvert->nd_ra_flags_reserved & ND_RA_FLAG_OTHER) &&
      !zif->rtadv.AdvOtherConfigFlag)
    {
      zlog_warn("%s(%u): Rx RA - our AdvOtherConfigFlag doesn't agree with %s",
                ifp->name, ifp->ifindex, addr_str);
    }

  if ((radvert->nd_ra_reachable && zif->rtadv.AdvReachableTime) &&
      (ntohl(radvert->nd_ra_reachable) != zif->rtadv.AdvReachableTime))
    {
      zlog_warn("%s(%u): Rx RA - our AdvReachableTime doesn't agree with %s",
                ifp->name, ifp->ifindex, addr_str);
    }

  if ((radvert->nd_ra_retransmit && zif->rtadv.AdvRetransTimer) &&
      (ntohl(radvert->nd_ra_retransmit) != (unsigned int)zif->rtadv.AdvRetransTimer))
    {
      zlog_warn("%s(%u): Rx RA - our AdvRetransTimer doesn't agree with %s",
                ifp->name, ifp->ifindex, addr_str);
    }

  /* Create entry for neighbor if not known. */
  p.family = AF_INET6;
  IPV6_ADDR_COPY (&p.u.prefix, &addr->sin6_addr);
  p.prefixlen = IPV6_MAX_PREFIXLEN;

  if (!nbr_connected_check(ifp, &p))
    nbr_connected_add_ipv6 (ifp, &addr->sin6_addr);
}


static void
rtadv_process_packet (u_char *buf, unsigned int len, ifindex_t ifindex, int hoplimit,
                      struct sockaddr_in6 *from, struct zebra_ns *zns)
{
  struct icmp6_hdr *icmph;
  struct interface *ifp;
  struct zebra_if *zif;
  char addr_str[INET6_ADDRSTRLEN];

  inet_ntop (AF_INET6, &from->sin6_addr, addr_str, INET6_ADDRSTRLEN);

  /* Interface search. */
  ifp = if_lookup_by_index_per_ns (zns, ifindex);
  if (ifp == NULL)
    {
      zlog_warn ("RA/RS received on unknown IF %u from %s",
                 ifindex, addr_str);
      return;
    }

  if (IS_ZEBRA_DEBUG_PACKET)
    zlog_debug ("%s(%u): Rx RA/RS len %d from %s",
                ifp->name, ifp->ifindex, len, addr_str);

  if (if_is_loopback (ifp) ||
      CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK))
    return;

  /* Check interface configuration. */
  zif = ifp->info;
  if (! zif->rtadv.AdvSendAdvertisements)
    return;

  /* ICMP message length check. */
  if (len < sizeof (struct icmp6_hdr))
    {
      zlog_warn ("%s(%u): Rx RA with Invalid ICMPV6 packet length %d",
                 ifp->name, ifp->ifindex, len);
      return;
    }

  icmph = (struct icmp6_hdr *) buf;

  /* ICMP message type check. */
  if (icmph->icmp6_type != ND_ROUTER_SOLICIT &&
      icmph->icmp6_type != ND_ROUTER_ADVERT)
    {
      zlog_warn ("%s(%u): Rx RA - Unwanted ICMPV6 message type %d",
                 ifp->name, ifp->ifindex, icmph->icmp6_type);
      return;
    }

  /* Hoplimit check. */
  if (hoplimit >= 0 && hoplimit != 255)
    {
      zlog_warn ("%s(%u): Rx RA - Invalid hoplimit %d",
		 ifp->name, ifp->ifindex, hoplimit);
      return;
    }

  /* Check ICMP message type. */
  if (icmph->icmp6_type == ND_ROUTER_SOLICIT)
    rtadv_process_solicit (ifp);
  else if (icmph->icmp6_type == ND_ROUTER_ADVERT)
    rtadv_process_advert (buf, len, ifp, from);

  return;
}

static int
rtadv_read (struct thread *thread)
{
  int sock;
  int len;
  u_char buf[RTADV_MSG_SIZE];
  struct sockaddr_in6 from;
  ifindex_t ifindex = 0;
  int hoplimit = -1;
  struct zebra_ns *zns = THREAD_ARG (thread);

  sock = THREAD_FD (thread);
  zns->rtadv.ra_read = NULL;

  /* Register myself. */
  rtadv_event (zns, RTADV_READ, sock);

  len = rtadv_recv_packet (zns, sock, buf, sizeof (buf), &from, &ifindex, &hoplimit);

  if (len < 0) 
    {
      zlog_warn ("RA/RS recv failed, socket %u error %s",
                 sock, safe_strerror (errno));
      return len;
    }

  rtadv_process_packet (buf, (unsigned)len, ifindex, hoplimit, &from, zns);

  return 0;
}

static int
rtadv_make_socket (void)
{
  int sock;
  int ret = 0;
  struct icmp6_filter filter;

  if ( zserv_privs.change (ZPRIVS_RAISE) )
       zlog_err ("rtadv_make_socket: could not raise privs, %s",
                  safe_strerror (errno) );
                  
  sock = socket (AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);

  if ( zserv_privs.change (ZPRIVS_LOWER) )
       zlog_err ("rtadv_make_socket: could not lower privs, %s",
       			 safe_strerror (errno) );

  if (sock < 0)
    {
      close (sock);
      return -1;
    }

  ret = setsockopt_ipv6_pktinfo (sock, 1);
  if (ret < 0)
    {
      close (sock);
      return ret;
    }
  ret = setsockopt_ipv6_multicast_loop (sock, 0);
  if (ret < 0)
    {
      close (sock);
      return ret;
    }
  ret = setsockopt_ipv6_unicast_hops (sock, 255);
  if (ret < 0)
    {
      close (sock);
      return ret;
    }
  ret = setsockopt_ipv6_multicast_hops (sock, 255);
  if (ret < 0)
    {
      close (sock);
      return ret;
    }
  ret = setsockopt_ipv6_hoplimit (sock, 1);
  if (ret < 0)
    {
      close (sock);
      return ret;
    }

  ICMP6_FILTER_SETBLOCKALL(&filter);
  ICMP6_FILTER_SETPASS (ND_ROUTER_SOLICIT, &filter);
  ICMP6_FILTER_SETPASS (ND_ROUTER_ADVERT, &filter);

  ret = setsockopt (sock, IPPROTO_ICMPV6, ICMP6_FILTER, &filter,
		    sizeof (struct icmp6_filter));
  if (ret < 0)
    {
      zlog_info ("ICMP6_FILTER set fail: %s", safe_strerror (errno));
      return ret;
    }

  return sock;
}

static struct rtadv_prefix *
rtadv_prefix_new (void)
{
  return XCALLOC (MTYPE_RTADV_PREFIX, sizeof (struct rtadv_prefix));
}

static void
rtadv_prefix_free (struct rtadv_prefix *rtadv_prefix)
{
  XFREE (MTYPE_RTADV_PREFIX, rtadv_prefix);
}

static struct rtadv_prefix *
rtadv_prefix_lookup (struct list *rplist, struct prefix_ipv6 *p)
{
  struct listnode *node;
  struct rtadv_prefix *rprefix;

  for (ALL_LIST_ELEMENTS_RO (rplist, node, rprefix))
    if (prefix_same ((struct prefix *) &rprefix->prefix, (struct prefix *) p))
      return rprefix;
  return NULL;
}

static struct rtadv_prefix *
rtadv_prefix_get (struct list *rplist, struct prefix_ipv6 *p)
{
  struct rtadv_prefix *rprefix;
  
  rprefix = rtadv_prefix_lookup (rplist, p);
  if (rprefix)
    return rprefix;

  rprefix = rtadv_prefix_new ();
  memcpy (&rprefix->prefix, p, sizeof (struct prefix_ipv6));
  listnode_add (rplist, rprefix);

  return rprefix;
}

static void
rtadv_prefix_set (struct zebra_if *zif, struct rtadv_prefix *rp)
{
  struct rtadv_prefix *rprefix;
  
  rprefix = rtadv_prefix_get (zif->rtadv.AdvPrefixList, &rp->prefix);

  /* Set parameters. */
  rprefix->AdvValidLifetime = rp->AdvValidLifetime;
  rprefix->AdvPreferredLifetime = rp->AdvPreferredLifetime;
  rprefix->AdvOnLinkFlag = rp->AdvOnLinkFlag;
  rprefix->AdvAutonomousFlag = rp->AdvAutonomousFlag;
  rprefix->AdvRouterAddressFlag = rp->AdvRouterAddressFlag;
}

static int
rtadv_prefix_reset (struct zebra_if *zif, struct rtadv_prefix *rp)
{
  struct rtadv_prefix *rprefix;
  
  rprefix = rtadv_prefix_lookup (zif->rtadv.AdvPrefixList, &rp->prefix);
  if (rprefix != NULL)
    {
      listnode_delete (zif->rtadv.AdvPrefixList, (void *) rprefix);
      rtadv_prefix_free (rprefix);
      return 1;
    }
  else
    return 0;
}

static void
ipv6_nd_suppress_ra_set (struct interface *ifp, ipv6_nd_suppress_ra_status status)
{
  struct zebra_if *zif;
  struct zebra_vrf *zvrf;
  struct zebra_ns *zns;

  zif = ifp->info;
  zvrf = vrf_info_lookup (ifp->vrf_id);
  zns = zvrf->zns;

  if (status == RA_SUPPRESS)
    {
      /* RA is currently enabled */
      if (zif->rtadv.AdvSendAdvertisements)
        {
          zif->rtadv.AdvSendAdvertisements = 0;
          zif->rtadv.AdvIntervalTimer = 0;
          zns->rtadv.adv_if_count--;

          if_leave_all_router (zns->rtadv.sock, ifp);

          if (zns->rtadv.adv_if_count == 0)
            rtadv_event (zns, RTADV_STOP, 0);
        }
    }
  else
    {
      if (! zif->rtadv.AdvSendAdvertisements)
        {
          zif->rtadv.AdvSendAdvertisements = 1;
          zif->rtadv.AdvIntervalTimer = 0;
          zns->rtadv.adv_if_count++;

          if (zif->rtadv.MaxRtrAdvInterval >= 1000)
            {
              /* Enable Fast RA only when RA interval is in secs */
              zif->rtadv.inFastRexmit = 1;
              zif->rtadv.NumFastReXmitsRemain = RTADV_NUM_FAST_REXMITS;
            }

          if_join_all_router (zns->rtadv.sock, ifp);

          if (zns->rtadv.adv_if_count == 1)
            rtadv_event (zns, RTADV_START, zns->rtadv.sock);
        }
    }
}

/*
 * Handle client (BGP) message to enable or disable IPv6 RA on an interface.
 * Note that while the client could request RA on an interface on which the
 * operator has not enabled RA, RA won't be disabled upon client request
 * if the operator has explicitly enabled RA. The enable request can also
 * specify a RA interval (in seconds).
 */
void
zebra_interface_radv_set (struct zserv *client, int sock, u_short length,
                          struct zebra_vrf *zvrf, int enable)
{
  struct stream *s;
  unsigned int ifindex;
  struct interface *ifp;
  struct zebra_if *zif;
  int ra_interval;

  s = client->ibuf;

  /* Get interface index and RA interval. */
  ifindex = stream_getl (s);
  ra_interval = stream_getl (s);

  if (IS_ZEBRA_DEBUG_EVENT)
    zlog_debug("%u: IF %u RA %s from client %s, interval %ds",
               zvrf_id (zvrf), ifindex, enable ? "enable" : "disable",
               zebra_route_string(client->proto), ra_interval);

  /* Locate interface and check VRF match. */
  ifp = if_lookup_by_index_per_ns (zebra_ns_lookup (NS_DEFAULT), ifindex);
  if (!ifp)
    {
      zlog_warn("%u: IF %u RA %s client %s - interface unknown",
               zvrf_id (zvrf), ifindex, enable ? "enable" : "disable",
               zebra_route_string(client->proto));
      return;
    }
  if (ifp->vrf_id != zvrf_id (zvrf))
    {
      zlog_warn("%u: IF %u RA %s client %s - VRF mismatch, IF VRF %u",
               zvrf_id (zvrf), ifindex, enable ? "enable" : "disable",
               zebra_route_string(client->proto), ifp->vrf_id);
      return;
    }

  zif = ifp->info;
  if (enable)
    {
      ipv6_nd_suppress_ra_set (ifp, RA_ENABLE);
      if (ra_interval &&
          (ra_interval * 1000) < zif->rtadv.MaxRtrAdvInterval)
        zif->rtadv.MaxRtrAdvInterval = ra_interval * 1000;
    }
  else
    {
      if (!zif->rtadv.configured)
        {
          zif->rtadv.MaxRtrAdvInterval = RTADV_MAX_RTR_ADV_INTERVAL;
          ipv6_nd_suppress_ra_set (ifp, RA_SUPPRESS);
        }
    }
}

DEFUN (ipv6_nd_suppress_ra,
       ipv6_nd_suppress_ra_cmd,
       "ipv6 nd suppress-ra",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Suppress Router Advertisement\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  if (if_is_loopback (ifp) ||
      CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK))
    {
      vty_outln (vty,
                 "Cannot configure IPv6 Router Advertisements on this  interface");
      return CMD_WARNING_CONFIG_FAILED;
    }

  ipv6_nd_suppress_ra_set (ifp, RA_SUPPRESS);
  zif->rtadv.configured = 0;
  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_suppress_ra,
       no_ipv6_nd_suppress_ra_cmd,
       "no ipv6 nd suppress-ra",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Suppress Router Advertisement\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  if (if_is_loopback (ifp) ||
      CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK))
    {
      vty_outln (vty,
                 "Cannot configure IPv6 Router Advertisements on this interface");
      return CMD_WARNING_CONFIG_FAILED;
    }

  ipv6_nd_suppress_ra_set (ifp, RA_ENABLE);
  zif->rtadv.configured = 1;
  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_ra_interval_msec,
       ipv6_nd_ra_interval_msec_cmd,
       "ipv6 nd ra-interval msec (70-1800000)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Router Advertisement interval\n"
       "Router Advertisement interval in milliseconds\n"
       "Router Advertisement interval in milliseconds\n")
{
  int idx_number = 4;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  unsigned interval;
  struct zebra_if *zif = ifp->info;
  struct zebra_vrf *zvrf = vrf_info_lookup (ifp->vrf_id);
  struct zebra_ns *zns;

  zns = zvrf->zns;
  interval = strtoul(argv[idx_number]->arg, NULL, 10);
  if ((zif->rtadv.AdvDefaultLifetime != -1 && interval > (unsigned)zif->rtadv.AdvDefaultLifetime * 1000))
  {
    vty_outln (vty,
               "This ra-interval would conflict with configured ra-lifetime!");
    return CMD_WARNING_CONFIG_FAILED;
  }

  if (zif->rtadv.MaxRtrAdvInterval % 1000)
    zns->rtadv.adv_msec_if_count--;

  if (interval % 1000)
    zns->rtadv.adv_msec_if_count++;
  
  zif->rtadv.MaxRtrAdvInterval = interval;
  zif->rtadv.MinRtrAdvInterval = 0.33 * interval;
  zif->rtadv.AdvIntervalTimer = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_ra_interval,
       ipv6_nd_ra_interval_cmd,
       "ipv6 nd ra-interval (1-1800)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Router Advertisement interval\n"
       "Router Advertisement interval in seconds\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  unsigned interval;
  struct zebra_if *zif = ifp->info;
  struct zebra_vrf *zvrf = vrf_info_lookup (ifp->vrf_id);
  struct zebra_ns *zns;

  zns = zvrf->zns;
  interval = strtoul(argv[idx_number]->arg, NULL, 10);
  if ((zif->rtadv.AdvDefaultLifetime != -1 && interval > (unsigned)zif->rtadv.AdvDefaultLifetime))
  {
    vty_outln (vty,
               "This ra-interval would conflict with configured ra-lifetime!");
    return CMD_WARNING_CONFIG_FAILED;
  }

  if (zif->rtadv.MaxRtrAdvInterval % 1000)
    zns->rtadv.adv_msec_if_count--;
	
  /* convert to milliseconds */
  interval = interval * 1000; 
	
  zif->rtadv.MaxRtrAdvInterval = interval;
  zif->rtadv.MinRtrAdvInterval = 0.33 * interval;
  zif->rtadv.AdvIntervalTimer = 0;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_ra_interval,
       no_ipv6_nd_ra_interval_cmd,
       "no ipv6 nd ra-interval [<(1-1800)|msec (1-1800000)>]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Router Advertisement interval\n"
       "Router Advertisement interval in seconds\n"
       "Specify millisecond router advertisement interval\n"
       "Router Advertisement interval in milliseconds\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  struct zebra_vrf *zvrf;
  struct zebra_ns *zns;

  zvrf = vrf_info_lookup (ifp->vrf_id);
  zns = zvrf->zns;

  if (zif->rtadv.MaxRtrAdvInterval % 1000)
    zns->rtadv.adv_msec_if_count--;
  
  zif->rtadv.MaxRtrAdvInterval = RTADV_MAX_RTR_ADV_INTERVAL;
  zif->rtadv.MinRtrAdvInterval = RTADV_MIN_RTR_ADV_INTERVAL;
  zif->rtadv.AdvIntervalTimer = zif->rtadv.MaxRtrAdvInterval;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_ra_lifetime,
       ipv6_nd_ra_lifetime_cmd,
       "ipv6 nd ra-lifetime (0-9000)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Router lifetime\n"
       "Router lifetime in seconds (0 stands for a non-default gw)\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  int lifetime;

  lifetime = strtoul(argv[idx_number]->arg, NULL, 10);

  /* The value to be placed in the Router Lifetime field
   * of Router Advertisements sent from the interface,
   * in seconds.  MUST be either zero or between
   * MaxRtrAdvInterval and 9000 seconds. -- RFC4861, 6.2.1 */
  if ((lifetime != 0 && lifetime * 1000 < zif->rtadv.MaxRtrAdvInterval))
    {
      vty_outln (vty,
                 "This ra-lifetime would conflict with configured ra-interval");
      return CMD_WARNING_CONFIG_FAILED;
    }

  zif->rtadv.AdvDefaultLifetime = lifetime;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_ra_lifetime,
       no_ipv6_nd_ra_lifetime_cmd,
       "no ipv6 nd ra-lifetime [(0-9000)]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Router lifetime\n"
       "Router lifetime in seconds (0 stands for a non-default gw)\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvDefaultLifetime = -1;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_reachable_time,
       ipv6_nd_reachable_time_cmd,
       "ipv6 nd reachable-time (1-3600000)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Reachable time\n"
       "Reachable time in milliseconds\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  zif->rtadv.AdvReachableTime = strtoul(argv[idx_number]->arg, NULL, 10);
  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_reachable_time,
       no_ipv6_nd_reachable_time_cmd,
       "no ipv6 nd reachable-time [(1-3600000)]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Reachable time\n"
       "Reachable time in milliseconds\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvReachableTime = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_homeagent_preference,
       ipv6_nd_homeagent_preference_cmd,
       "ipv6 nd home-agent-preference (0-65535)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent preference\n"
       "preference value (default is 0, least preferred)\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  zif->rtadv.HomeAgentPreference = strtoul(argv[idx_number]->arg, NULL, 10);
  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_homeagent_preference,
       no_ipv6_nd_homeagent_preference_cmd,
       "no ipv6 nd home-agent-preference [(0-65535)]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent preference\n"
       "preference value (default is 0, least preferred)\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.HomeAgentPreference = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_homeagent_lifetime,
       ipv6_nd_homeagent_lifetime_cmd,
       "ipv6 nd home-agent-lifetime (0-65520)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent lifetime\n"
       "Home Agent lifetime in seconds (0 to track ra-lifetime)\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  zif->rtadv.HomeAgentLifetime = strtoul(argv[idx_number]->arg, NULL, 10);
  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_homeagent_lifetime,
       no_ipv6_nd_homeagent_lifetime_cmd,
       "no ipv6 nd home-agent-lifetime [(0-65520)]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent lifetime\n"
       "Home Agent lifetime in seconds (0 to track ra-lifetime)\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.HomeAgentLifetime = -1;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_managed_config_flag,
       ipv6_nd_managed_config_flag_cmd,
       "ipv6 nd managed-config-flag",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Managed address configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvManagedFlag = 1;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_managed_config_flag,
       no_ipv6_nd_managed_config_flag_cmd,
       "no ipv6 nd managed-config-flag",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Managed address configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvManagedFlag = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_homeagent_config_flag,
       ipv6_nd_homeagent_config_flag_cmd,
       "ipv6 nd home-agent-config-flag",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvHomeAgentFlag = 1;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_homeagent_config_flag,
       no_ipv6_nd_homeagent_config_flag_cmd,
       "no ipv6 nd home-agent-config-flag",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Home Agent configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvHomeAgentFlag = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_adv_interval_config_option,
       ipv6_nd_adv_interval_config_option_cmd,
       "ipv6 nd adv-interval-option",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Advertisement Interval Option\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvIntervalOption = 1;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_adv_interval_config_option,
       no_ipv6_nd_adv_interval_config_option_cmd,
       "no ipv6 nd adv-interval-option",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Advertisement Interval Option\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvIntervalOption = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_other_config_flag,
       ipv6_nd_other_config_flag_cmd,
       "ipv6 nd other-config-flag",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Other statefull configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvOtherConfigFlag = 1;

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_other_config_flag,
       no_ipv6_nd_other_config_flag_cmd,
       "no ipv6 nd other-config-flag",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Other statefull configuration flag\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.AdvOtherConfigFlag = 0;

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_prefix,
       ipv6_nd_prefix_cmd,
       "ipv6 nd prefix X:X::X:X/M [<(0-4294967295)|infinite> <(0-4294967295)|infinite>] [<router-address|off-link [no-autoconfig]|no-autoconfig [off-link]>]",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Prefix information\n"
       "IPv6 prefix\n"
       "Valid lifetime in seconds\n"
       "Infinite valid lifetime\n"
       "Preferred lifetime in seconds\n"
       "Infinite preferred lifetime\n"
       "Set Router Address flag\n"
       "Do not use prefix for onlink determination\n"
       "Do not use prefix for autoconfiguration\n"
       "Do not use prefix for autoconfiguration\n"
       "Do not use prefix for onlink determination\n")
{
  /* prelude */
  char *prefix = argv[3]->arg;
  int lifetimes = (argc > 4) && (argv[4]->type == RANGE_TKN || strmatch (argv[4]->text, "infinite"));
  int routeropts = lifetimes ? argc > 6 : argc > 4;

  int idx_routeropts = routeropts ? (lifetimes ? 6 : 4) : 0;

  char *lifetime = NULL, *preflifetime = NULL;
  int routeraddr = 0, offlink = 0, noautoconf = 0;
  if (lifetimes)
  {
    lifetime     = argv[4]->type == RANGE_TKN ? argv[4]->arg : argv[4]->text;
    preflifetime = argv[5]->type == RANGE_TKN ? argv[5]->arg : argv[5]->text;
  }
  if (routeropts)
  {
    routeraddr = strmatch (argv[idx_routeropts]->text, "router-address");
    if (!routeraddr)
    {
      offlink    = (argc > idx_routeropts + 1 || strmatch (argv[idx_routeropts]->text, "off-link"));
      noautoconf = (argc > idx_routeropts + 1 || strmatch (argv[idx_routeropts]->text, "no-autoconfig"));
    }
  }

  /* business */
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zebra_if = ifp->info;
  int ret;
  struct rtadv_prefix rp;

  ret = str2prefix_ipv6 (prefix, &rp.prefix);
  if (!ret)
    {
      vty_outln (vty, "Malformed IPv6 prefix");
      return CMD_WARNING_CONFIG_FAILED;
    }
  apply_mask_ipv6 (&rp.prefix); /* RFC4861 4.6.2 */
  rp.AdvOnLinkFlag = !offlink;
  rp.AdvAutonomousFlag = !noautoconf;
  rp.AdvRouterAddressFlag = routeraddr;
  rp.AdvValidLifetime = RTADV_VALID_LIFETIME;
  rp.AdvPreferredLifetime = RTADV_PREFERRED_LIFETIME;

  if (lifetimes)
  {
    rp.AdvValidLifetime     = strmatch (lifetime, "infinite") ? UINT32_MAX : strtoll (lifetime, NULL, 10);
    rp.AdvPreferredLifetime = strmatch (preflifetime, "infinite") ? UINT32_MAX : strtoll (preflifetime, NULL, 10);
    if (rp.AdvPreferredLifetime > rp.AdvValidLifetime)
      {
        vty_outln (vty, "Invalid preferred lifetime");
        return CMD_WARNING_CONFIG_FAILED;
      }
  }

  rtadv_prefix_set (zebra_if, &rp);

  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_prefix,
       no_ipv6_nd_prefix_cmd,
       "no ipv6 nd prefix X:X::X:X/M [<(0-4294967295)|infinite> <(0-4294967295)|infinite>] [<router-address|off-link [no-autoconfig]|no-autoconfig [off-link]>]",
        NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Prefix information\n"
       "IPv6 prefix\n"
       "Valid lifetime in seconds\n"
       "Infinite valid lifetime\n"
       "Preferred lifetime in seconds\n"
       "Infinite preferred lifetime\n"
       "Set Router Address flag\n"
       "Do not use prefix for onlink determination\n"
       "Do not use prefix for autoconfiguration\n"
       "Do not use prefix for autoconfiguration\n"
       "Do not use prefix for onlink determination\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zebra_if = ifp->info;
  int ret;
  struct rtadv_prefix rp;
  char *prefix = argv[4]->arg;

  ret = str2prefix_ipv6 (prefix, &rp.prefix);
  if (!ret)
    {
      vty_outln (vty, "Malformed IPv6 prefix");
      return CMD_WARNING_CONFIG_FAILED;
    }
  apply_mask_ipv6 (&rp.prefix); /* RFC4861 4.6.2 */

  ret = rtadv_prefix_reset (zebra_if, &rp);
  if (!ret)
    {
      vty_outln (vty, "Non-existant IPv6 prefix");
      return CMD_WARNING_CONFIG_FAILED;
    }

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_router_preference,
       ipv6_nd_router_preference_cmd,
       "ipv6 nd router-preference <high|medium|low>",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Default router preference\n"
       "High default router preference\n"
       "Medium default router preference (default)\n"
       "Low default router preference\n")
{
  int idx_high_medium_low = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  int i = 0;

  while (0 != rtadv_pref_strs[i])
    {
      if (strncmp (argv[idx_high_medium_low]->arg, rtadv_pref_strs[i], 1) == 0)
	{
	  zif->rtadv.DefaultPreference = i;
	  return CMD_SUCCESS;
	}
      i++;
    }

  return CMD_ERR_NO_MATCH;
}

DEFUN (no_ipv6_nd_router_preference,
       no_ipv6_nd_router_preference_cmd,
       "no ipv6 nd router-preference [<high|medium|low>]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Default router preference\n"
       "High default router preference\n"
       "Medium default router preference (default)\n"
       "Low default router preference\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;

  zif->rtadv.DefaultPreference = RTADV_PREF_MEDIUM; /* Default per RFC4191. */

  return CMD_SUCCESS;
}

DEFUN (ipv6_nd_mtu,
       ipv6_nd_mtu_cmd,
       "ipv6 nd mtu (1-65535)",
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Advertised MTU\n"
       "MTU in bytes\n")
{
  int idx_number = 3;
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  zif->rtadv.AdvLinkMTU = strtoul(argv[idx_number]->arg, NULL, 10);
  return CMD_SUCCESS;
}

DEFUN (no_ipv6_nd_mtu,
       no_ipv6_nd_mtu_cmd,
       "no ipv6 nd mtu [(1-65535)]",
       NO_STR
       "Interface IPv6 config commands\n"
       "Neighbor discovery\n"
       "Advertised MTU\n"
       "MTU in bytes\n")
{
  VTY_DECLVAR_CONTEXT (interface, ifp);
  struct zebra_if *zif = ifp->info;
  zif->rtadv.AdvLinkMTU = 0;
  return CMD_SUCCESS;
}


/* Write configuration about router advertisement. */
void
rtadv_config_write (struct vty *vty, struct interface *ifp)
{
  struct zebra_if *zif;
  struct listnode *node;
  struct rtadv_prefix *rprefix;
  char buf[PREFIX_STRLEN];
  int interval;

  zif = ifp->info;

  if (!(if_is_loopback (ifp) ||
        CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK)))
    {
      if (zif->rtadv.AdvSendAdvertisements)
        vty_outln (vty, " no ipv6 nd suppress-ra");
    }
  
  interval = zif->rtadv.MaxRtrAdvInterval;
  if (interval % 1000)
    vty_outln (vty, " ipv6 nd ra-interval msec %d",interval);
  else
    if (interval != RTADV_MAX_RTR_ADV_INTERVAL)
      vty_outln (vty, " ipv6 nd ra-interval %d",interval / 1000);

  if (zif->rtadv.AdvIntervalOption)
    vty_outln (vty, " ipv6 nd adv-interval-option");

  if (zif->rtadv.AdvDefaultLifetime != -1)
    vty_outln (vty, " ipv6 nd ra-lifetime %d",zif->rtadv.AdvDefaultLifetime);

  if (zif->rtadv.HomeAgentPreference)
    vty_outln (vty, " ipv6 nd home-agent-preference %u",
	     zif->rtadv.HomeAgentPreference);

  if (zif->rtadv.HomeAgentLifetime != -1)
    vty_outln (vty, " ipv6 nd home-agent-lifetime %u",
	     zif->rtadv.HomeAgentLifetime);

  if (zif->rtadv.AdvHomeAgentFlag)
    vty_outln (vty, " ipv6 nd home-agent-config-flag");

  if (zif->rtadv.AdvReachableTime)
    vty_outln (vty, " ipv6 nd reachable-time %d",
               zif->rtadv.AdvReachableTime);

  if (zif->rtadv.AdvManagedFlag)
    vty_outln (vty, " ipv6 nd managed-config-flag");

  if (zif->rtadv.AdvOtherConfigFlag)
    vty_outln (vty, " ipv6 nd other-config-flag");

  if (zif->rtadv.DefaultPreference != RTADV_PREF_MEDIUM)
    vty_outln (vty, " ipv6 nd router-preference %s",
	     rtadv_pref_strs[zif->rtadv.DefaultPreference]);

  if (zif->rtadv.AdvLinkMTU)
    vty_outln (vty, " ipv6 nd mtu %d", zif->rtadv.AdvLinkMTU);

  for (ALL_LIST_ELEMENTS_RO (zif->rtadv.AdvPrefixList, node, rprefix))
    {
      vty_out (vty, " ipv6 nd prefix %s",
               prefix2str (&rprefix->prefix, buf, sizeof(buf)));
      if ((rprefix->AdvValidLifetime != RTADV_VALID_LIFETIME) || 
	  (rprefix->AdvPreferredLifetime != RTADV_PREFERRED_LIFETIME))
	{
	  if (rprefix->AdvValidLifetime == UINT32_MAX)
	    vty_out (vty, " infinite");
	  else
	    vty_out (vty, " %u", rprefix->AdvValidLifetime);
	  if (rprefix->AdvPreferredLifetime == UINT32_MAX)
	    vty_out (vty, " infinite");
	  else
	    vty_out (vty, " %u", rprefix->AdvPreferredLifetime);
	}
      if (!rprefix->AdvOnLinkFlag)
	vty_out (vty, " off-link");
      if (!rprefix->AdvAutonomousFlag)
	vty_out (vty, " no-autoconfig");
      if (rprefix->AdvRouterAddressFlag)
	vty_out (vty, " router-address");
      vty_out (vty, VTYNL);
    }
}


static void
rtadv_event (struct zebra_ns *zns, enum rtadv_event event, int val)
{
  struct rtadv *rtadv = &zns->rtadv;

  switch (event)
    {
    case RTADV_START:
      thread_add_read(zebrad.master, rtadv_read, zns, val, &rtadv->ra_read);
      thread_add_event(zebrad.master, rtadv_timer, zns, 0, &rtadv->ra_timer);
      break;
    case RTADV_STOP:
      if (rtadv->ra_timer)
	{
	  thread_cancel (rtadv->ra_timer);
	  rtadv->ra_timer = NULL;
	}
      if (rtadv->ra_read)
	{
	  thread_cancel (rtadv->ra_read);
	  rtadv->ra_read = NULL;
	}
      break;
    case RTADV_TIMER:
      thread_add_timer(zebrad.master, rtadv_timer, zns, val, &rtadv->ra_timer);
      break;
    case RTADV_TIMER_MSEC:
      thread_add_timer_msec(zebrad.master, rtadv_timer, zns, val,
                            &rtadv->ra_timer);
      break;
    case RTADV_READ:
      thread_add_read(zebrad.master, rtadv_read, zns, val, &rtadv->ra_read);
      break;
    default:
      break;
    }
  return;
}

void
rtadv_init (struct zebra_ns *zns)
{
  zns->rtadv.sock = rtadv_make_socket ();
}

void
rtadv_terminate (struct zebra_ns *zns)
{
  rtadv_event (zns, RTADV_STOP, 0);
  if (zns->rtadv.sock >= 0)
    {
      close (zns->rtadv.sock);
      zns->rtadv.sock = -1;
    }

  zns->rtadv.adv_if_count = 0;
  zns->rtadv.adv_msec_if_count = 0;
}

void
rtadv_cmd_init (void)
{
  install_element (INTERFACE_NODE, &ipv6_nd_suppress_ra_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_suppress_ra_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_ra_interval_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_ra_interval_msec_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_ra_interval_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_ra_lifetime_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_ra_lifetime_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_reachable_time_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_reachable_time_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_managed_config_flag_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_managed_config_flag_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_other_config_flag_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_other_config_flag_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_homeagent_config_flag_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_homeagent_config_flag_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_homeagent_preference_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_homeagent_preference_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_homeagent_lifetime_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_homeagent_lifetime_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_adv_interval_config_option_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_adv_interval_config_option_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_prefix_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_prefix_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_router_preference_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_router_preference_cmd);
  install_element (INTERFACE_NODE, &ipv6_nd_mtu_cmd);
  install_element (INTERFACE_NODE, &no_ipv6_nd_mtu_cmd);
}

static int
if_join_all_router (int sock, struct interface *ifp)
{
  int ret;

  struct ipv6_mreq mreq;

  memset (&mreq, 0, sizeof (struct ipv6_mreq));
  inet_pton (AF_INET6, ALLROUTER, &mreq.ipv6mr_multiaddr);
  mreq.ipv6mr_interface = ifp->ifindex;

  ret = setsockopt (sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, 
		    (char *) &mreq, sizeof mreq);
  if (ret < 0)
    zlog_warn ("%s(%u): Failed to join group, socket %u error %s",
               ifp->name, ifp->ifindex, sock, safe_strerror (errno));

  if (IS_ZEBRA_DEBUG_EVENT)
    zlog_debug ("%s(%u): Join All-Routers multicast group, socket %u",
                ifp->name, ifp->ifindex, sock);

  return 0;
}

static int
if_leave_all_router (int sock, struct interface *ifp)
{
  int ret;

  struct ipv6_mreq mreq;

  memset (&mreq, 0, sizeof (struct ipv6_mreq));
  inet_pton (AF_INET6, ALLROUTER, &mreq.ipv6mr_multiaddr);
  mreq.ipv6mr_interface = ifp->ifindex;

  ret = setsockopt (sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, 
		    (char *) &mreq, sizeof mreq);
  if (ret < 0)
    zlog_warn ("%s(%u): Failed to leave group, socket %u error %s",
               ifp->name, ifp->ifindex, sock,safe_strerror (errno));

  if (IS_ZEBRA_DEBUG_EVENT)
    zlog_debug ("%s(%u): Leave All-Routers multicast group, socket %u",
                ifp->name, ifp->ifindex, sock);

  return 0;
}

#else
void
rtadv_init (struct zebra_ns *zns)
{
  /* Empty.*/;
}
void
rtadv_terminate (struct zebra_ns *zns)
{
  /* Empty.*/;
}
void
rtadv_cmd_init (void)
{
  /* Empty.*/;
}
#endif /* HAVE_RTADV */
