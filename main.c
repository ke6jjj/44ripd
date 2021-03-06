/*
 * This is an OpenBSD daemon for a modified version of the RIPv2
 * protocol (RFC 2453).  It is designed to handle route and
 * tunnel maintenance for the AMPR amateur radio network
 * (IPv4 44/8: http://www.ampr.org).  Note that the AMPRNet is a
 * mesh: each site sets up IPENCAP tunnels to (almost) all other
 * sites.
 *
 * The daemon listens on a multicast socket for UDP datagrams
 * directed to the RIP port.  It discards packets that are not
 * valid, authenticated RIP packets (though authentication is
 * simply string comparison against a plaintext password: it is
 * not particularly strong and could be trivially broken).  It
 * maintains an internal copy of the AMPRNet routing table as
 * well as a set of active tunnels.
 *
 * After processing a RIP packet, the daemon walks through the
 * routing table, looking for routes to expire.  If a route
 * expires it is noted for removal from the table.  Expiration
 * time is much greater than the expected interval between
 * RIP broadcasts.
 *
 * Routes keep a reference to a tunnel.  When a route is added
 * that refers to an non-existent tunnel, the tunnel is created
 * and set up.  If a route referring to a tunnel is removed or
 * changed to a different tunnel, the tunnel's reference count
 * is decremented. If a tunnel's reference count drops to zero,
 * it is torn down and removed.
 *
 * Each tunnel corresponds to a virtual IP encapsulation
 * interface; see gif(4) for details.  The daemon dynamically
 * creates and destroys these interfaces as required.  A bitmap
 * of active interfaces is kept and the lowest unused interface
 * number is always allocated when a new tunnel is created.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dat.h"
#include "lib.h"
#include "log.h"
#include "rip.h"
#include "sys.h"

static int init(int argc, char *argv[]);
static void learnsys(int rtable);
static void cleanup(void);
static void learn_interface_callback(const char *name, int num,
    uint32_t outer_local, uint32_t outer_remote, uint32_t inner_local,
    uint32_t inner_remote, void *arg);
static void learn_route_callback(uint32_t ipnet, uint32_t mask,
    int isaddr, uint32_t dstaddr, const char *dstif, void *arg);
static int set_expire_time(uint32_t key, size_t keylen, void *routep,
    void *arg);
static unsigned int strnum(const char *restrict str);
static void riptide(int sd);
static void ripresponse(RIPResponse *response, time_t now);
static Route *mkroute(uint32_t ipnet, uint32_t subnetmask, uint32_t gateway);
static Tunnel *mktunnel(uint32_t outer_local, uint32_t outer_remote,
    uint32_t inner_local, uint32_t inner_remote);
static void alloctunif(Tunnel *tunnel, Bitvec *interfaces);
static void unlinkroute(Tunnel *tunnel, Route *route);
static void linkroute(Tunnel *tunnel, Route *route);
static void walkexpired(time_t now);
static int destroy(uint32_t key, size_t keylen, void *routep, void *unused);
static void collapse(Tunnel *tunnel);
static int expire(uint32_t key, size_t keylen, void *routep, void *statep);
static void usage(const char *restrict prog);
static int tunnelfindbyname(uint32_t key, size_t keylen, void *datum,
   void *arg);
static int tunnelfindbydest(uint32_t key, size_t keylen, void *datum,
   void *arg);
static int fix_overlaps(uint32_t key, size_t keylen, void *tunnelp, void *arg);
static int find_empty(uint32_t key, size_t keylen, void *tunnelp, void *arg);
static int unlink_redundant(uint32_t key, size_t keylen, void *routep,
   void *arg);
static void dump_all(FILE *out);

typedef struct SystemBuildContext SystemBuildContext;
typedef struct TunnelFindByNameParams TunnelFindByNameParams;
typedef struct TunnelFindByDestParams TunnelFindByDestParams;
typedef struct UnlinkRedundantParams UnlinkRedundantParams;
typedef struct TunnelList TunnelList;

struct SystemBuildContext {
	IPMap *acceptableroutes;
	IPMap *tunnels;
	IPMap *routes;
	const Bitvec *staticinterfaces;
	Bitvec *interfaces;
};

struct TunnelFindByNameParams {
	const char *name;
	Tunnel *tunnel;
};

struct TunnelFindByDestParams {
	uint32_t destaddr;
	Tunnel *tunnel;
};

struct UnlinkRedundantParams {
	Tunnel *tunnel;
	Route *parent;
};

struct TunnelList {
	Tunnel *tunnel;
	TunnelList *next;
};

enum {
	CIDR_HOST = 32,
	RIPV2_PORT = 520,
	DEFAULT_ROUTE_TABLE = 44,
	TIMEOUT = 7*24*60*60,	// 7 days
};

static const char *RIPV2_GROUP = "224.0.0.9";
static const char *PASSWORD = "pLaInTeXtpAsSwD";

static void * const IGNORE = (void *)0x10;	// Arbitrary.
static void * const ACCEPT = (void *)0x11;	// Arbitrary.

static IPMap *acceptableroutes;
static IPMap *routes;
static IPMap *tunnels;
static Bitvec *interfaces;
static Bitvec *staticinterfaces;

static const char *prog;
static uint32_t local_outer_addr;
static uint32_t local_inner_addr;
static int routetable_bind, routetable_create;
static int read_from_file;

int
main(int argc, char *argv[])
{
	int sd;

	sd = init(argc, argv);
	for (;;)
		riptide(sd);
	close(sd);

	return 0;
}

int
init(int argc, char *argv[])
{
	const char *local_outer_ip, *local_inner_ip;
	char *slash;
	int sd, ch, daemonize, dump;
	size_t acceptcount;
	struct in_addr addr;

	slash = strrchr(argv[0], '/');
	prog = (slash == NULL) ? argv[0] : slash + 1;
	daemonize = 1;
	dump = 0;
	read_from_file = 0;
	interfaces = mkbitvec();
	staticinterfaces = mkbitvec();
	routetable_create = DEFAULT_ROUTE_TABLE;
	routetable_bind = DEFAULT_ROUTE_TABLE;
	local_outer_ip = NULL;
	local_inner_ip = NULL;
	routes = mkipmap();
	tunnels = mkipmap();
	acceptableroutes = mkipmap();
	acceptcount = 0;
	while ((ch = getopt(argc, argv, "A:B:DI:T:df:s:")) != -1) {
		switch (ch) {
		case 'd':
			daemonize = 0;
			break;
		case 'D':
			dump = 1;
			break;
		case 'T':
			routetable_create = strnum(optarg);
			break;
		case 'B':
			routetable_bind = strnum(optarg);
			break;
		case 'A':
		case 'I': {
			void *ignore_accept = (ch == 'A' ? ACCEPT : IGNORE);
			struct in_addr iroute;
			size_t icidr;

			slash = strchr(optarg, '/');
			if (slash == NULL)
				fatal("Bad route (use CIDR): %s", optarg);
			*slash++ = '\0';
			if (inet_aton(optarg, &iroute) != 1)
				fatal("Bad route addr: %s", optarg);
			iroute.s_addr = ntohl(iroute.s_addr);
			icidr = strnum(slash);
			ipmapinsert(acceptableroutes, iroute.s_addr, icidr,
			    ignore_accept);
			acceptcount++;
			break;
		}
		case 's': {
			unsigned int ifnum = strnum(optarg);
			bitset(staticinterfaces, ifnum);
			bitset(interfaces, ifnum);
			break;
		}
		case 'f': {
			if (read_from_file)
				fatal("Can only read from one file.");
			read_from_file = 1;
			sd = open(optarg, O_RDONLY);
			if (sd < 0)
				fatal("Can't open '%s'", optarg);
			break;
		}
		case '?':
		case 'h':
		default:
			usage(prog);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 2)
		usage(prog);

	if (acceptcount == 0)
		// Accept everything by default
		ipmapinsert(acceptableroutes, 0, 0, ACCEPT);

	local_outer_ip = argv[0];
	local_inner_ip = argv[1];

	inet_pton(AF_INET, local_outer_ip, &addr);
	local_outer_addr = ntohl(addr.s_addr);

	inet_pton(AF_INET, local_inner_ip, &addr);
	local_inner_addr = ntohl(addr.s_addr);

	initlog();

	learnsys(routetable_create);

	if (dump) {
		dump_all(stdout);
		exit(0);
	}

	initsys(routetable_create);

	if (!read_from_file)
		sd = initsock(RIPV2_GROUP, RIPV2_PORT, routetable_bind);

	cleanup();

	if (daemonize) {
		const int no_chdir = 0;
		const int no_close = 0;
		daemon(no_chdir, no_close);
	}

	return sd;
}

enum {
	MAX_NUM = (1 << 20),
};

static void
learnsys(int rtable)
{
	SystemBuildContext ctx;
	ctx.acceptableroutes = acceptableroutes;
	ctx.tunnels = tunnels;
	ctx.routes = routes;
	ctx.staticinterfaces = staticinterfaces;
	ctx.interfaces = interfaces;

	//
	// Build an in-memory view of all the tunnels and routes on the
	// system that appear to be part of the AMPR mesh.
	//
	discover(rtable, learn_interface_callback, learn_route_callback, &ctx);

	//
	// Find and remove redundant routes from in-memory view.
	//
	ipmapdo(tunnels, fix_overlaps, &ctx);

	//
	// Give reasonable expiration times for the routes we've discovered.
	//
	time_t expire = time(NULL);
	expire += TIMEOUT;
	ipmapdo(routes, set_expire_time, &expire);
}

static void
cleanup(void)
{
	//
	// Find all tunnels that serve no networks at all.
	//
	TunnelList *emptytunnel = NULL;
	ipmapdo(tunnels, find_empty, &emptytunnel);

	//
	// Bring down the empty tunnels.
	//
	while (emptytunnel != NULL) {
		TunnelList *next = emptytunnel->next;
		collapse(emptytunnel->tunnel);
		free(emptytunnel);
		emptytunnel = next;
	}
}

static void
learn_interface_callback(const char *name, int num,
    uint32_t outer_local, uint32_t outer_remote, uint32_t inner_local,
    uint32_t inner_remote, void *arg)
{
	SystemBuildContext *ctx = arg;

	if (bitget(ctx->staticinterfaces, num))
		return;

	assert(bitget(ctx->interfaces, num) == 0);

	void *accept = ipmapnearest(ctx->acceptableroutes, inner_remote,
	    CIDR_HOST);
	if (accept != ACCEPT)
		fatal("interface %s has unacceptable destination", name);

	Tunnel *tunnel = mktunnel(outer_local, outer_remote, inner_local,
	    inner_remote);

	tunnel->ifnum = num;
	strncpy(tunnel->ifname, name, sizeof(tunnel->ifname)-1);

	if (ipmapinsert(ctx->tunnels, outer_remote, CIDR_HOST, tunnel)
	    != tunnel)
	{
		fatal("interface %s duplicates another interface", name);
	}
	bitset(ctx->interfaces, num);
}

static void
learn_route_callback(uint32_t ipnet, uint32_t mask,
    int isaddr, uint32_t destaddr, const char *destif, void *arg)
{
	SystemBuildContext *ctx = arg;
	char net[INET_ADDRSTRLEN];
	Tunnel *tunnel;

	ipaddrstr(ipnet, net);
	int cidr = netmask2cidr(mask);
	if (cidr == -1) {
		fatal("unusual netmask found in routed network %s/0x%08" PRIx32,
		    net, mask);
	}

	if (isaddr) {
		TunnelFindByDestParams params;
		params.destaddr = destaddr;
		params.tunnel = NULL;
		ipmapdo(tunnels, tunnelfindbydest, &params);
		tunnel = params.tunnel;
	} else {
		TunnelFindByNameParams params;
		params.name = destif;
		params.tunnel = NULL;
		ipmapdo(tunnels, tunnelfindbyname, &params);
		tunnel = params.tunnel;
	}

	void *accept = ipmapnearest(ctx->acceptableroutes, ipnet, cidr);

	if (tunnel == NULL) {
		if (accept == ACCEPT) {
			fatal("acceptable network %s/%d routed to "
			    "unknown destination", net, cidr);
		}
		return;
	}
	if (accept != ACCEPT) {
		fatal("unacceptable network %s/%d found with managed tunnel",
		    net, cidr);
	}
	
	Route *route = mkroute(ipnet, mask, tunnel->outer_remote);
	Route *existing = ipmapinsert(ctx->routes, ipnet, cidr, route);

	if (existing != route) {
		if (existing->ipnet != route->ipnet ||
		    existing->subnetmask != route->subnetmask ||
		    existing->gateway != route->gateway)
		{
			char othernet[INET_ADDRSTRLEN],
			     othergw[INET_ADDRSTRLEN],
			     gw[INET_ADDRSTRLEN];
			int othercidr = netmask2cidr(existing->subnetmask);
			ipaddrstr(route->gateway, gw);
			ipaddrstr(existing->ipnet, othernet);
			ipaddrstr(existing->gateway, othergw);
			fatal("duplicate route for %s/%d->%s detected (other "
			      "%s/%d->%s", net, cidr, gw, othernet, othercidr,
			      othergw);
		}
		free(route);
		return;
	}

	linkroute(tunnel, route);
}

static int
tunnelfindbydest(uint32_t key, size_t keylen, void *datum, void *arg)
{
	Tunnel *tunnel = datum;
	TunnelFindByDestParams *params = arg;

	if (tunnel->inner_remote == params->destaddr) {
		params->tunnel = tunnel;
		return 1;
	}

	return 0;
}

static int
tunnelfindbyname(uint32_t key, size_t keylen, void *datum, void *arg)
{
	Tunnel *tunnel = datum;
	TunnelFindByNameParams *params = arg;

	if (strcmp(tunnel->ifname, params->name) == 0) {
		params->tunnel = tunnel;
		return 1;
	}

	return 0;
}

static int
set_expire_time(uint32_t key, size_t keylen, void *routep, void *arg)
{
	Route *route = routep;
	time_t *when = arg;

	route->expires = *when;

	return 0;
}

static void
dummy_free(void *unused)
{
	(void)unused;
}

//
// Work around a general problem that the operating system automatically
// inserts hosts routes to tunnel inner destinations even if we later
// assign an entire network to be routed through that tunnel, and the
// network covers the single host route. These automatically inserted
// routes need to be detected and removed from the discovered route
// list.
//
static int
fix_overlaps(uint32_t key, size_t keylen, void *tunnelp, void *arg)
{
	Tunnel *tunnel = tunnelp;
	IPMap *coverage = mkipmap();

	for (Route *route = tunnel->routes; route; route = route->rnext) {
		int cidr = netmask2cidr(route->subnetmask);
		ipmapinsert(coverage, route->ipnet, cidr, route);
	}

	UnlinkRedundantParams p;
	p.tunnel = tunnel;
	p.parent = NULL;

	ipmapdotopdown(coverage, unlink_redundant, &p);

	freeipmap(coverage, dummy_free);

	return 0;
}

//
// Find tunnels with no allocated routes.
//
static int
find_empty(uint32_t key, size_t keylen, void *tunnelp, void *tlst_ptr)
{
	Tunnel *tunnel = tunnelp;
	TunnelList **tlst = tlst_ptr;

	if (tunnel->routes == NULL) {
		assert(tunnel->nref == 0);
		TunnelList *entry = malloc(sizeof(TunnelList));
		if (entry == NULL)
			fatal("malloc");
		entry->tunnel = tunnel;
		entry->next = *tlst;
		*tlst = entry;
	} else {
		assert(tunnel->nref > 0);
	}

	return 0;
}

static int
unlink_redundant(uint32_t key, size_t keylen, void *routep, void *arg)
{
	Route *route = routep;
	UnlinkRedundantParams *p = arg;
	Route *parent = p->parent;

	if (parent != NULL && (parent->ipnet & parent->subnetmask) ==
	    (route->ipnet & parent->subnetmask))
	{
		// This route is redundant.
		unlinkroute(p->tunnel, route);
		return 0;
	}
	p->parent = route;
	return 0;
}

unsigned int
strnum(const char *restrict str)
{
	char *ep;
	unsigned long r;

	ep = NULL;
	r = strtoul(str, &ep, 10);
	if (ep != NULL && *ep != '\0')
		fatal("bad unsigned integer: %s", str);
	if (r > MAX_NUM)
		fatal("integer range error: %s", str);

	return (unsigned int)r;
}


void
riptide(int sd)
{
	struct sockaddr *rem;
	struct sockaddr_in remote;
	socklen_t remotelen;
	ssize_t n;
	time_t now;
	RIPPacket pkt;
	octet packet[IP_MAXPACKET];

	memset(&remote, 0, sizeof(remote));
	remotelen = 0;
	rem = (struct sockaddr *)&remote;
	if (read_from_file) {
		n = read(sd, packet, sizeof(packet));
		if (n == 0)
			fatal("done");
	} else {
		n = recvfrom(sd, packet, sizeof(packet), 0, rem, &remotelen);
	}
	if (n < 0)
		fatal("socket error");
	memset(&pkt, 0, sizeof(pkt));
	if (parserippkt(packet, n, &pkt) < 0) {
		error("packet parse error");
		return;
	}
	if (verifyripauth(&pkt, PASSWORD) < 0) {
		error("packet authentication failed");
		return;
	}
	now = time(NULL);
	for (int k = 0; k < pkt.nresponse; k++) {
		RIPResponse response;
		memset(&response, 0, sizeof(response));
		if (parseripresponse(&pkt, k, &response) < 0) {
			notice("bad response, index %d", k);
			continue;
		}
		ripresponse(&response, now);
	}
	walkexpired(now);
}

void
ripresponse(RIPResponse *response, time_t now)
{
	Route *route;
	void *acceptance;
	Tunnel *tunnel;
	int cidr;
	char proute[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN];

	cidr = netmask2cidr(response->subnetmask);
	ipaddrstr(response->ipaddr, proute);
	ipaddrstr(response->nexthop, gw);
	debug("RIPv2 response: %s/%d -> %s", proute, cidr, gw);
	if (response->ipaddr & ~response->subnetmask)
		error("route ipaddr %s has more bits than netmask, %d",
		    proute, cidr);
	response->ipaddr &= response->subnetmask;
	if (response->nexthop == local_outer_addr) {
		info("skipping route for %s/%d to local address",
		    proute, cidr);
		return;
	}
	if ((response->nexthop & response->subnetmask) == response->ipaddr) {
		info("skipping gateway inside of subnet (%s/%d -> %s)",
		    proute, cidr, gw);
		return;
	}
	acceptance = ipmapnearest(acceptableroutes, response->ipaddr, cidr);
	if (acceptance == NULL || acceptance != ACCEPT) {
		info("skipping ignored network %s/%d", proute, cidr);
		return;
	}
	tunnel = ipmapfind(tunnels, response->nexthop, CIDR_HOST);
	if (tunnel == NULL) {
		debug("creating new tunnel for %s/%d -> %s", proute, cidr,
		    gw);
		tunnel = mktunnel(local_outer_addr, response->nexthop,
		    local_inner_addr, response->ipaddr);
		alloctunif(tunnel, interfaces);
		uptunnel(tunnel, routetable_create);
		ipmapinsert(tunnels, response->nexthop, CIDR_HOST, tunnel);
	}
	route = ipmapfind(routes, response->ipaddr, cidr);
	if (route == NULL) {
		Route *cover = ipmapnearest(routes, response->ipaddr, cidr);
		if (cover != NULL) {
			char covernet[INET_ADDRSTRLEN];
			ipaddrstr(cover->ipnet, covernet);
			int covercidr = netmask2cidr(cover->subnetmask);	
			if (cover->tunnel == tunnel) {
				info("skipping network %s/%d because it is "
				    "served by %s/%d", proute, cidr, covernet,
				    covercidr);
				return;
			}
			debug("branching network %s/%d off of %s/d",
			    proute, cidr, covernet, covercidr);
		}
		route = mkroute(
		    response->ipaddr,
		    response->subnetmask,
		    response->nexthop);
		ipmapinsert(routes, route->ipnet, cidr, route);
		info("Added route %s/%d -> %s", proute, cidr, gw);
	}
	if (route->tunnel != tunnel) {
		// The route is new or moved to a different tunnel.
		if (route->tunnel == NULL) {
			debug("no tunnel for %s/%d, adding new route via %s",
			    proute, cidr, gw, tunnel->ifname);
			addroute(route, tunnel, routetable_create);
		} else {
			debug("tunnel for %s/%d changed. %s -> %s",
			    proute, cidr, route->tunnel->ifname,
			    tunnel->ifname);
			chroute(route, tunnel, routetable_create);
		}
		unlinkroute(route->tunnel, route);
		collapse(route->tunnel);
		linkroute(tunnel, route);
	}
	route->expires = now + TIMEOUT;
}

Route *
mkroute(uint32_t ipnet, uint32_t subnetmask, uint32_t gateway)
{
	Route *route;

	route = calloc(1, sizeof(*route));
	if (route == NULL)
		fatal("malloc");
	route->ipnet = ipnet;
	route->subnetmask = subnetmask;
	route->gateway = gateway;

	return route;
}

Tunnel *
mktunnel(uint32_t outer_local, uint32_t outer_remote, uint32_t inner_local,
    uint32_t inner_remote)
{
	Tunnel *tunnel;

	tunnel = calloc(1, sizeof(*tunnel));
	if (tunnel == NULL)
		fatal("malloc");
	tunnel->outer_local = outer_local;
	tunnel->outer_remote = outer_remote;
	tunnel->inner_local = inner_local;
	tunnel->inner_remote = inner_remote;

	return tunnel;
}

void
alloctunif(Tunnel *tunnel, Bitvec *interfaces)
{
	size_t ifnum;

	ifnum = nextbit(interfaces);
	tunnel->ifnum = ifnum;
	snprintf(tunnel->ifname, sizeof(tunnel->ifname), "gif%zu", ifnum);
	bitset(interfaces, ifnum);
	info("Allocating tunnel interface %s", tunnel->ifname);
}

void
unlinkroute(Tunnel *tunnel, Route *route)
{
	if (tunnel == NULL)
		return;
	for (Route *prev = NULL, *tmp = tunnel->routes;
	    tmp != NULL;
	    prev = tmp, tmp = tmp->rnext)
	{
		if (route->ipnet == tmp->ipnet &&
		    route->subnetmask == tmp->subnetmask)
		{
			if (prev == NULL)
				tunnel->routes = tmp->rnext;
			else
				prev->rnext = tmp->rnext;
			route->gateway = 0;
			--tunnel->nref;
			break;
		}
	}
}

void
linkroute(Tunnel *tunnel, Route *route)
{
	route->rnext = tunnel->routes;
	tunnel->routes = route;
	route->tunnel = tunnel;
	route->gateway = tunnel->outer_remote;
	++tunnel->nref;
}

typedef struct WalkState WalkState;
struct WalkState {
	time_t now;
	IPMap *deleting;
};

void
walkexpired(time_t now)
{
	WalkState state = { now, NULL };

	ipmapdo(routes, expire, &state);
	if (state.deleting != NULL) {
		ipmapdo(state.deleting, destroy, NULL);
		freeipmap(state.deleting, free);
	}
}

int
expire(uint32_t key, size_t keylen, void *routep, void *statep)
{
	Route *route = routep;
	WalkState *state = statep;
	int cidr;
	char proute[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN];

	if (route->expires > state->now)
		return 0;

	if (state->deleting == NULL)
		state->deleting = mkipmap(); 
	cidr = netmask2cidr(route->subnetmask);
	assert(cidr == keylen);
	ipaddrstr(route->ipnet, proute);
	ipaddrstr(route->gateway, gw);
	info("Expiring route %s/%d -> %s", proute, cidr, gw);
	ipmapinsert(state->deleting, key, keylen, route);

	return 0;
}

int
destroy(uint32_t key, size_t keylen, void *routep, void *unused)
{
	Route *route = routep;
	Tunnel *tunnel;
	void *datum;
	int cidr;
	char proute[INET_ADDRSTRLEN], gw[INET_ADDRSTRLEN];

	(void)unused;
	if (route == NULL)
		return 0;
	cidr = netmask2cidr(route->subnetmask);
	assert(cidr == keylen);
	ipaddrstr(route->ipnet, proute);
	ipaddrstr(route->gateway, gw);
	info("Destroying route %s/%d -> %s", proute, cidr, gw);
	datum = ipmapremove(routes, key, keylen);
	assert(datum == route);
	tunnel = route->tunnel;
	assert(tunnel != NULL);
	rmroute(route, routetable_create);
	unlinkroute(tunnel, route);
	collapse(tunnel);

	return 0;
}

void
collapse(Tunnel *tunnel)
{
	if (tunnel == NULL)
		return;
	assert(tunnel->nref >= 0);
	if (tunnel->nref == 0) {
		void *datum = ipmapremove(tunnels, tunnel->outer_remote,
		    CIDR_HOST);
		assert(datum == tunnel);
		info("Tearing down tunnel interface %s", tunnel->ifname);
		downtunnel(tunnel);
		bitclr(interfaces, tunnel->ifnum);
		free(tunnel);
	}
}

static int
dump_tunnel(uint32_t key, size_t keylen, void *tunnelp, void *arg)
{
	Tunnel *tunnel = tunnelp;
	FILE *out = arg;

	char outer_local[INET_ADDRSTRLEN], outer_remote[INET_ADDRSTRLEN],
	     inner_local[INET_ADDRSTRLEN], inner_remote[INET_ADDRSTRLEN];

	ipaddrstr(tunnel->outer_local, outer_local);
	ipaddrstr(tunnel->outer_remote, outer_remote);
	ipaddrstr(tunnel->inner_local, inner_local);
	ipaddrstr(tunnel->inner_remote, inner_remote);

	fprintf(out, "Tunnel interface %s:\n"
	              "\tOuter %s -> %s\n"
	              "\tInner %s -> %s\n"
	              "\tRouted networks:\n",
	    tunnel->ifname, outer_local, outer_remote, inner_local,
	    inner_remote);

	Route *route;
	for (route = tunnel->routes; route; route = route->rnext) {
		char net[INET_ADDRSTRLEN];
		size_t cidr;

		assert(route->tunnel == tunnel);

		ipaddrstr(route->ipnet, net);
		cidr = netmask2cidr(route->subnetmask);

		fprintf(out, "\t\t%s/%zd\n", net, cidr);
	}

	return 0;
}

static int
dump_accept_reject(uint32_t key, size_t keylen, void *accept, void *arg)
{
	FILE *out = arg;
	char net[INET_ADDRSTRLEN];

	ipaddrstr(key, net);

	fprintf(out, "\t%s/%zd -> %s\n", net, keylen,
	    accept == ACCEPT ? "ACCEPT" : "REJECT");

	return 0;
}

static void
dump_all(FILE *out)
{
	fputs("Acceptance policy:\n", out);
	ipmapdotopdown(acceptableroutes, dump_accept_reject, out);
	ipmapdo(tunnels, dump_tunnel, out);
}

void
usage(const char *restrict prog)
{
	fprintf(stderr,
	    "Usage: %s [ -d | -D ] [ -T <create_rtable> ] [ -I <ignorespec> ] "
	        "[ -A <acceptspec> ] [ -s <static_ifnum> ] [ -f <testfile> ] "
	        "[ -B <bind_rtable> ] <local-outer-ip> <local-ampr-ip>\n",
	    prog);
	exit(EXIT_FAILURE);
}
