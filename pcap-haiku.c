/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		James Woodcock
 */


#include "config.h"
#include "pcap-int.h"

#include <OS.h>

#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/utsname.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// IFT_TUN was renamed to IFT_TUNNEL in the master branch after R1/beta4 (the
// integer value didn't change).  Even though IFT_TUN is a no-op in versions
// that define it, for the time being it is desirable to support compiling
// libpcap on versions with the old macro and using it on later versions that
// support tunnel interfaces.
#ifndef IFT_TUNNEL
#define IFT_TUNNEL IFT_TUN
#endif

/*
 * Private data for capturing on Haiku sockets.
 */
struct pcap_haiku {
	struct pcap_stat	stat;
	int aux_socket;
	struct ifreq ifreq;
	// The original state of the promiscuous mode at the activation time,
	// if the capture should be run in promiscuous mode.
	int orig_promisc;
};


static int
pcap_read_haiku(pcap_t* handle, int maxPackets _U_, pcap_handler callback,
	u_char* userdata)
{
	// Receive a single packet

	u_char* buffer = (u_char*)handle->buffer + handle->offset;
	struct sockaddr_dl from;
	ssize_t bytesReceived;
	do {
		if (handle->break_loop) {
			handle->break_loop = 0;
			return PCAP_ERROR_BREAK;
		}

		socklen_t fromLength = sizeof(from);
		bytesReceived = recvfrom(handle->fd, buffer, handle->bufsize, MSG_TRUNC,
			(struct sockaddr*)&from, &fromLength);
	} while (bytesReceived < 0 && errno == B_INTERRUPTED);

	bigtime_t ts = real_time_clock_usecs();

	if (bytesReceived < 0) {
		if (errno == B_WOULD_BLOCK) {
			// there is no packet for us
			return 0;
		}

		pcapint_fmt_errmsg_for_errno(handle->errbuf, PCAP_ERRBUF_SIZE,
		    errno, "recvfrom");
		return PCAP_ERROR;
	}

	struct pcap_haiku* handlep = (struct pcap_haiku*)handle->priv;
	handlep->stat.ps_recv++;

	if (bytesReceived > handle->bufsize) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		         "recvfrom() returned %ld, which exceeds the buffer size %u",
		         bytesReceived, handle->bufsize);
		return PCAP_ERROR;
	}
	bpf_u_int32 captureLength = (bpf_u_int32)bytesReceived;

	// run the packet filter
	if (handle->fcode.bf_insns) {
		// NB: pcapint_filter() for both length arguments takes the
		// return value of recvfrom(), not the snapshot length of the
		// pcap_t handle.
		if (pcapint_filter(handle->fcode.bf_insns, buffer, captureLength,
				captureLength) == 0) {
			// packet got rejected
			handlep->stat.ps_drop++;
			return 0;
		}
	}

	// fill in pcap_header
	struct pcap_pkthdr header;
	header.caplen = captureLength <= (bpf_u_int32)handle->snapshot ?
	                captureLength :
	                (bpf_u_int32)handle->snapshot;
	header.len = captureLength;
	header.ts.tv_usec = ts % 1000000;
	header.ts.tv_sec = ts / 1000000;
	// TODO: get timing from packet!!!

	/* Call the user supplied callback function */
	callback(userdata, &header, buffer);
	return 1;
}


static int
dgram_socket(const int af, char *errbuf)
{
	int ret = socket(af, SOCK_DGRAM, 0);
	if (ret < 0) {
		pcapint_fmt_errmsg_for_errno(errbuf, PCAP_ERRBUF_SIZE, errno,
		    "socket");
		return PCAP_ERROR;
	}
	return ret;
}


static int
ioctl_ifreq(const int fd, const unsigned long op, const char *name,
             struct ifreq *ifreq, char *errbuf)
{
	if (ioctl(fd, op, ifreq, sizeof(struct ifreq)) < 0) {
		pcapint_fmt_errmsg_for_errno(errbuf, PCAP_ERRBUF_SIZE, errno,
		    "%s", name);
		return PCAP_ERROR;
	}
	return 0;
}


static int
get_promisc(pcap_t *handle)
{
	struct pcap_haiku *handlep = (struct pcap_haiku *)handle->priv;
	// SIOCGIFFLAGS would work fine for AF_LINK too.
	if (ioctl_ifreq(handlep->aux_socket, SIOCGIFFLAGS, "SIOCGIFFLAGS",
	                &handlep->ifreq, handle->errbuf) < 0)
		return PCAP_ERROR;
	return (handlep->ifreq.ifr_flags & IFF_PROMISC) != 0;
}


static int
set_promisc(pcap_t *handle, const int enable)
{
	struct pcap_haiku *handlep = (struct pcap_haiku *)handle->priv;
	if (enable)
		handlep->ifreq.ifr_flags |= IFF_PROMISC;
	else
		handlep->ifreq.ifr_flags &= ~IFF_PROMISC;
	// SIOCSIFFLAGS works for AF_INET, but not for AF_LINK.
	return ioctl_ifreq(handlep->aux_socket, SIOCSIFFLAGS, "SIOCSIFFLAGS",
	                   &handlep->ifreq, handle->errbuf);
}


static void
pcap_cleanup_haiku(pcap_t *handle)
{
	if (handle->fd >= 0) {
		close(handle->fd);
		handle->fd = -1;
		handle->selectable_fd = -1;
	}

	struct pcap_haiku *handlep = (struct pcap_haiku *)handle->priv;
	if (handlep->aux_socket >= 0) {
		// Closing the sockets has no effect on IFF_PROMISC, hence the
		// need to restore the original state on one hand and the
		// possibility of clash with other processes managing the same
		// interface flag.  Unset promiscuous mode iff the activation
		// function had set it and it is still set now.
		if (handle->opt.promisc && ! handlep->orig_promisc &&
		    get_promisc(handle))
			(void)set_promisc(handle, 0);
		close(handlep->aux_socket);
		handlep->aux_socket = -1;
	}
}


static int
pcap_inject_haiku(pcap_t *handle, const void *buffer _U_, int size _U_)
{
	// we don't support injecting packets yet
	// TODO: use the AF_LINK protocol (we need another socket for this) to
	// inject the packets
	strlcpy(handle->errbuf, "Sending packets isn't supported yet",
		PCAP_ERRBUF_SIZE);
	return PCAP_ERROR;
}


static int
pcap_stats_haiku(pcap_t *handle, struct pcap_stat *stats)
{
	struct pcap_haiku* handlep = (struct pcap_haiku*)handle->priv;
	*stats = handlep->stat;
	// Now ps_recv and ps_drop are accurate, but ps_ifdrop still equals to
	// the snapshot value from the activation time.
	if (ioctl_ifreq(handlep->aux_socket, SIOCGIFSTATS, "SIOCGIFSTATS",
	                &handlep->ifreq, handle->errbuf) < 0)
		return PCAP_ERROR;
	// The result is subject to wrapping around the 32-bit integer space,
	// but that cannot be significantly improved as long as it has to fit
	// into a 32-bit member of pcap_stats.
	stats->ps_ifdrop = handlep->ifreq.ifr_stats.receive.dropped - stats->ps_ifdrop;
	return 0;
}


static int
pcap_activate_haiku(pcap_t *handle)
{
	struct pcap_haiku *handlep = (struct pcap_haiku *)handle->priv;
	int ret = PCAP_ERROR;

	// we need a socket to talk to the networking stack
	if ((handlep->aux_socket = dgram_socket(AF_INET, handle->errbuf)) < 0)
		goto error;

	// pcap_stats_haiku() will need a baseline for ps_ifdrop.
	// At the time of this writing SIOCGIFSTATS returns EINVAL for AF_LINK
	// sockets.
	if (ioctl_ifreq(handlep->aux_socket, SIOCGIFSTATS, "SIOCGIFSTATS",
	                &handlep->ifreq, handle->errbuf) < 0) {
		// Detect a non-existent network interface at least at the
		// first ioctl() use.
		if (errno == EINVAL)
			ret = PCAP_ERROR_NO_SUCH_DEVICE;
		goto error;
	}
	handlep->stat.ps_ifdrop = handlep->ifreq.ifr_stats.receive.dropped;

	// get link level interface for this interface
	if ((handle->fd = dgram_socket(AF_LINK, handle->errbuf)) < 0)
		goto error;

	// Derive a DLT from the interface type.
	// At the time of this writing SIOCGIFTYPE cannot be used for this
	// purpose: it returns EINVAL for AF_LINK sockets and sets ifr_type to
	// 0 for AF_INET sockets.  Use the same method as Haiku ifconfig does
	// (SIOCGIFADDR and AF_LINK).
	if (ioctl_ifreq(handle->fd, SIOCGIFADDR, "SIOCGIFADDR",
	                &handlep->ifreq, handle->errbuf) < 0)
		goto error;
	struct sockaddr_dl *sdl = (struct sockaddr_dl *)&handlep->ifreq.ifr_addr;
	if (sdl->sdl_family != AF_LINK) {
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		         "Got AF %d instead of AF_LINK for interface \"%s\".",
		         sdl->sdl_family, handle->opt.device);
		goto error;
	}
	switch (sdl->sdl_type) {
	case IFT_ETHER:
		// Ethernet on all versions, also tap (L2) mode tunnels on
		// versions after R1/beta4.
		handle->linktype = DLT_EN10MB;
		break;
	case IFT_TUNNEL:
		// Unused on R1/beta4 and earlier versions, tun (L3) mode
		// tunnels on later versions.
	case IFT_LOOP:
		// The loopback interface on all versions.
		// Both IFT_TUNNEL and IFT_LOOP prepended a dummy Ethernet
		// header until hrev57585: https://dev.haiku-os.org/ticket/18801
		handle->linktype = DLT_RAW;
		break;
	default:
		snprintf(handle->errbuf, PCAP_ERRBUF_SIZE,
		         "Unknown interface type 0x%0x for interface \"%s\".",
		         sdl->sdl_type, handle->opt.device);
		goto error;
	}

	// start monitoring
	if (ioctl_ifreq(handle->fd, SIOCSPACKETCAP, "SIOCSPACKETCAP",
	                &handlep->ifreq, handle->errbuf) < 0)
		goto error;

	handle->selectable_fd = handle->fd;
	handle->read_op = pcap_read_haiku;
	handle->setfilter_op = pcapint_install_bpf_program; /* no kernel filtering */
	handle->inject_op = pcap_inject_haiku;
	handle->stats_op = pcap_stats_haiku;
	handle->cleanup_op = pcap_cleanup_haiku;

	// use default hooks where possible
	handle->getnonblock_op = pcapint_getnonblock_fd;
	handle->setnonblock_op = pcapint_setnonblock_fd;

	/*
	 * Turn a negative snapshot value (invalid), a snapshot value of
	 * 0 (unspecified), or a value bigger than the normal maximum
	 * value, into the maximum allowed value.
	 *
	 * If some application really *needs* a bigger snapshot
	 * length, we should just increase MAXIMUM_SNAPLEN.
	 */
	if (handle->snapshot <= 0 || handle->snapshot > MAXIMUM_SNAPLEN)
		handle->snapshot = MAXIMUM_SNAPLEN;

	handle->bufsize = 65536;
	// TODO: should be determined by interface MTU

	// allocate buffer for monitoring the device
	handle->buffer = (u_char*)malloc(handle->bufsize);
	if (handle->buffer == NULL) {
		pcapint_fmt_errmsg_for_errno(handle->errbuf, PCAP_ERRBUF_SIZE,
			errno, "buffer malloc");
		goto error;
	}

	handle->offset = 0;

	if (handle->opt.promisc) {
		// Set promiscuous mode iff required, in any case remember the
		// original state.
		if ((handlep->orig_promisc = get_promisc(handle)) < 0)
			goto error;
		if (! handlep->orig_promisc && set_promisc(handle, 1) < 0)
			return PCAP_WARNING_PROMISC_NOTSUP;
	}
	return 0;
error:
	pcap_cleanup_haiku(handle);
	return ret;
}


static int
validate_ifname(const char *device, char *errbuf)
{
	if (strlen(device) >= IF_NAMESIZE) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE,
		         "Interface name \"%s\" is too long.", device);
		return PCAP_ERROR;
	}
	return 0;
}


//	#pragma mark - pcap API


static int
can_be_bound(const char *name)
{
	if (strcmp(name, "loop") != 0)
		return 1;

	// In Haiku versions before hrev57010 the loopback interface allows to
	// start a capture, but the capture never receives any packets.
	//
	// Since compiling libpcap on one Haiku version and using the binary on
	// another seems to be commonplace, comparing B_HAIKU_VERSION at the
	// compile time would not always work as intended.  Let's at least
	// remove unsuitable well-known 64-bit versions (with or without
	// updates) from the problem space at run time.
	const char *badversions[] = {
		"hrev56578", // R1/beta4
		"hrev55182", // R1/beta3
		"hrev54154", // R1/beta2
		"hrev52295", // R1/beta1
		"hrev44702", // R1/alpha4
		NULL
	};
	struct utsname uts;
	(void)uname(&uts);
	for (const char **s = badversions; *s; s++)
		if (! strncmp(uts.version, *s, strlen(*s)))
			return 0;
	return 1;
}


pcap_t *
pcapint_create_interface(const char *device, char *errorBuffer)
{
	if (validate_ifname(device, errorBuffer) < 0)
		return NULL;
	if (! can_be_bound(device)) {
		snprintf(errorBuffer, PCAP_ERRBUF_SIZE,
		         "Interface \"%s\" does not support capturing traffic.", device);
		return NULL;
	}

	pcap_t* handle = PCAP_CREATE_COMMON(errorBuffer, struct pcap_haiku);
	if (handle == NULL) {
		pcapint_fmt_errmsg_for_errno(errorBuffer, PCAP_ERRBUF_SIZE,
		    errno, "malloc");
		return NULL;
	}
	handle->activate_op = pcap_activate_haiku;

	struct pcap_haiku *handlep = (struct pcap_haiku *)handle->priv;
	handlep->aux_socket = -1;
	strcpy(handlep->ifreq.ifr_name, device);

	return handle;
}


static int
get_if_flags(const char *name, bpf_u_int32 *flags, char *errbuf)
{
	if (validate_ifname(name, errbuf) < 0)
		return PCAP_ERROR;

	if (*flags & PCAP_IF_LOOPBACK ||
	    ! strncmp(name, "tun", strlen("tun")) ||
	    ! strncmp(name, "tap", strlen("tap"))) {
		/*
		 * Loopback devices aren't wireless, and "connected"/
		 * "disconnected" doesn't apply to them.
		 *
		 * Neither does it to tunnel interfaces.  A tun mode tunnel
		 * can be identified by the IFT_TUNNEL value, but tap mode
		 * tunnels and Ethernet interfaces both use IFT_ETHER, so let's
		 * use the interface name prefix until there is a better
		 * solution.
		 */
		*flags |= PCAP_IF_CONNECTION_STATUS_NOT_APPLICABLE;
		return (0);
	}

	int fd = dgram_socket(AF_LINK, errbuf);
	if (fd < 0)
		return PCAP_ERROR;
	struct ifreq ifreq;
	strcpy(ifreq.ifr_name, name);
	if (ioctl_ifreq(fd, SIOCGIFFLAGS, "SIOCGIFFLAGS", &ifreq, errbuf) < 0) {
		close(fd);
		return PCAP_ERROR;
	}
	*flags |= (ifreq.ifr_flags & IFF_LINK) ?
	          PCAP_IF_CONNECTION_STATUS_CONNECTED :
	          PCAP_IF_CONNECTION_STATUS_DISCONNECTED;
	close(fd);

	return (0);
}

int
pcapint_platform_finddevs(pcap_if_list_t* _allDevices, char* errorBuffer)
{
	return pcapint_findalldevs_interfaces(_allDevices, errorBuffer, can_be_bound,
		get_if_flags);
}

/*
 * Libpcap version string.
 */
const char *
pcap_lib_version(void)
{
	return (PCAP_VERSION_STRING);
}
