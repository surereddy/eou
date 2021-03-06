/*
 * Sean Manson COMP3301
 * Pseudodevice for ethernet over IP
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/socketvar.h>
#include <sys/ioctl.h>
#include <sys/task.h>
#include <sys/time.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>

#include <crypto/siphash.h>

#include <net/if_eou.h>

SLIST_HEAD(, eou_sock) eou_socklist;
SLIST_HEAD(, eou_softc) eou_sclist;

void	eouattach(int);
int	eou_clone_create(struct if_clone *, int);
int	eou_clone_destroy(struct ifnet *);

int	eouioctl(struct ifnet *, u_long, caddr_t);
void	eoustart(struct ifnet *);
void	eouping(void *);
void	eousend(void *);
void	eourecv(struct socket *, caddr_t, int);
int	eou_media_change(struct ifnet *);
void	eou_media_status(struct ifnet *, struct ifmediareq *);

int	eou_set_address(struct ifnet *, struct sockaddr_in *,
    struct sockaddr_in *);
int	eou_sock_create(struct eou_softc *, struct sockaddr_in *,
    struct sockaddr_in *, struct eou_sock **);
int	eou_sock_delete(struct eou_softc *);
void	eou_timeout_ping(void *);
void	eou_timeout_pong(void *);

void	eou_gen_mac(struct eou_pingpong *, uint8_t *);

/* globals */
struct if_clone	eou_cloner =
    IF_CLONE_INITIALIZER("eou", eou_clone_create, eou_clone_destroy);

void
eouattach(int neou)
{
	SLIST_INIT(&eou_socklist);
	SLIST_INIT(&eou_sclist);

	if_clone_attach(&eou_cloner);
}

int
eou_clone_create(struct if_clone *ifc, int unit)
{
	struct ifnet		*ifp;
	struct eou_softc	*sc;

	if ((sc = malloc(sizeof(*sc), M_DEVBUF, M_NOWAIT|M_ZERO)) == NULL)
		return (ENOMEM);

	/* basic address info settings */
	ifp = &sc->sc_ac.ac_if;
	snprintf(ifp->if_xname, sizeof(ifp->if_xname), "eou%d", unit);
	ifp->if_flags = IFF_SIMPLEX | IFF_BROADCAST | IFF_MULTICAST;
	ether_fakeaddr(ifp);
	ifp->if_softc = sc;
	/*ifp->if_capabilities = IFCAP_VLAN_MTU;*/
	
	/* softnet defaults */
	sc->sc_ifp = ifp;
	mq_init(&sc->sc_mq, 50, IPL_NET);
	task_set(&sc->sc_sndt, eousend, sc);
	task_set(&sc->sc_pingt, eouping, sc);
	sc->sc_network = 0;
	sc->sc_gotpong = 0;
	sc->sc_s = NULL;

	/* timeouts */
	timeout_set(&sc->sc_pingtmo, eou_timeout_ping, sc);
	timeout_set(&sc->sc_pongtmo, eou_timeout_pong, sc);

	/* send queue */
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	IFQ_SET_READY(&ifp->if_snd);

	/* handlers */
	ifp->if_ioctl = eouioctl;
	ifp->if_start = eoustart;

	/* media */
	ifmedia_init(&sc->sc_media, 0, eou_media_change, eou_media_status);
	ifmedia_add(&sc->sc_media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->sc_media, IFM_ETHER | IFM_AUTO);

	if_attach(ifp);
	ether_ifattach(ifp);

	/* add to overall list */
	SLIST_INSERT_HEAD(&eou_sclist, sc, sc_next);
	return (0);
}

int
eou_clone_destroy(struct ifnet *ifp)
{
	struct eou_softc	*sc = ifp->if_softc;
	int s, error = 0;

	s = splnet();

	/* desub */
	eou_set_address(ifp, NULL, NULL);
	
	/* remove sub structures */
	ifmedia_delete_instance(&sc->sc_media, IFM_INST_ANY);
	ether_ifdetach(ifp);
	if_detach(ifp);

	/* remove main structures */
	SLIST_REMOVE(&eou_sclist, sc, eou_softc, sc_next);
	free(sc, M_DEVBUF, sizeof(*sc));

	splx(s);
	return error;
}

/*
 * Handle IO commands given to us by ifconfig.
 */
/* ARGSUSED */
int
eouioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct eou_softc	*ent, *sc = (struct eou_softc *)ifp->if_softc;
	struct ifaddr		*ifa = (struct ifaddr *)data;
	struct ifreq		*ifr = (struct ifreq *)data;
	struct if_laddrreq	*lifr = (struct if_laddrreq *)data;
	struct proc		*p = curproc;
	int			 s, error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;
		if (ifa->ifa_addr->sa_family == AF_INET)
			arp_ifinit(&sc->sc_ac, ifa);
		/* FALLTHROUGH */

	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_UP) && sc->sc_s != NULL)
			ifp->if_flags |= IFF_RUNNING;
		else
			ifp->if_flags &= ~IFF_RUNNING;
		break;
		
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		/* should be already handled */
		break;

	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;
	
	case SIOCSLIFPHYADDR:
		if ((error = suser(p, 0)) != 0)
			break;
		s = splnet();
		error = eou_set_address(ifp, (struct sockaddr_in *)&lifr->addr,
		    (struct sockaddr_in *)&lifr->dstaddr);
		splx(s);
		break;

	case SIOCDIFPHYADDR:
		if ((error = suser(p, 0)) != 0)
			break;
		s = splnet();
		error = eou_set_address(ifp, NULL, NULL);
		splx(s);
		break;

	case SIOCGLIFPHYADDR:
		if (sc->sc_s == NULL) {
			error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
			break; /* socket only exists if tunnel exists */
		}
		bzero(&lifr->addr, sizeof(lifr->addr));
		bzero(&lifr->dstaddr, sizeof(lifr->dstaddr));
		memcpy(&lifr->addr, &sc->sc_s->so_src,
		    sizeof(sc->sc_s->so_src));
		memcpy(&lifr->dstaddr, &sc->sc_s->so_dst,
		    sizeof(sc->sc_s->so_dst));
		break;

	case SIOCSVNETID:
		if ((error = suser(p, 0)) != 0)
			break;
		if (ifr->ifr_vnetid < 0 || ifr->ifr_vnetid > 0x00ffffff) {
			error = EINVAL;
			break;
		}
		if (sc->sc_network == (uint32_t)ifr->ifr_vnetid)
			break; /* do nothing if setting to self */

		/* check if vnet exists for this socket */
		SLIST_FOREACH(ent, &eou_sclist, sc_next) {
			if (ent->sc_network == (uint32_t)ifr->ifr_vnetid) {
				return (EINVAL);
			}
		}

		/* set vnetid */
		s = splnet();
		sc->sc_network = (uint32_t)ifr->ifr_vnetid;
		sc->sc_gotpong = 0;
		timeout_add_sec(&sc->sc_pingtmo, 1);
		splx(s);
		break;

	case SIOCGVNETID:
		ifr->ifr_vnetid = (int)sc->sc_network;
		break;

	default:
		error = ether_ioctl(ifp, &sc->sc_ac, cmd, data);
		break;
	}

	return (error);
}

/*
 * Send a new ping message to the server.
 */
void
eouping(void *arg)
{
	struct eou_softc	*sc = arg;
	struct eou_pingpong	 msg;
	struct mbuf		*m;

	if (sc->sc_s == NULL)
		return;

	sc->sc_ifp->if_opackets++;
	
	/* create ping message */
	msg.hdr.eou_network = htonl(sc->sc_network);
	msg.hdr.eou_type = htons(EOU_T_PING);
	msg.utime = htobe64((uint64_t)time_second);
	arc4random_buf(msg.random, sizeof(msg.random));
	eou_gen_mac(&msg, msg.mac);

	/* add to send queue */
	MGETHDR(m, M_WAITOK, MT_DATA);
	m->m_len = 0;
	m->m_pkthdr.len = 0;
	m_copyback(m, 0, sizeof(msg), &msg, M_WAITOK);

	if (mq_enqueue(&sc->sc_mq, m) == 0)
		task_add(systq, &sc->sc_sndt);
	else {
		sc->sc_ifp->if_oerrors++;
		m_freem(m);
	}
}

/*
 * Start sending queued packets for this interface.
 */
void
eoustart(struct ifnet *ifp)
{
	struct eou_softc *sc = (struct eou_softc *)ifp->if_softc;
	struct eou_header *hdr;
	struct mbuf *m;
	int s;

	while (1) {
		/* get packets until none remain */
		s = splnet();
		IFQ_DEQUEUE(&ifp->if_snd, m);
		sc->sc_ifp->if_opackets++;
		splx(s);	
		if (m == NULL)
			break;

		/* ensure usable */	
		if ((ifp->if_flags & (IFF_OACTIVE | IFF_UP)) != IFF_UP ||
		    sc->sc_s == NULL)
			goto err;

		/* add packet header */
		M_PREPEND(m, sizeof(struct eou_header), M_NOWAIT);
		if (m == NULL)
			goto err;
		hdr = mtod(m, struct eou_header *);
		hdr->eou_network = htonl(sc->sc_network);
		hdr->eou_type = htons(EOU_T_DATA);

		/* add to send queue */
		if (mq_enqueue(&sc->sc_mq, m) != 0)
			goto err;

		continue;
		/* NOTREACHED */
	err:
		ifp->if_oerrors++;
		if (m != NULL)
			m_freem(m);
	}
	
	/* begin sending */
	task_add(systq, &sc->sc_sndt);
}

/*
 * Send our entire queue of mbufs to the socket in a process context.
 */
void
eousend(void *arg)
{
	struct eou_softc	*sc = arg;
	struct mbuf_list	 ml;
	struct mbuf		*m;
	int s, bytes, err = 0;

	ml_init(&ml);

	/* convert from queue to a list to write */
	s = splnet();
	mq_delist(&sc->sc_mq, &ml);
	splx(s);

	/* write all of these, so long as our socket is valid */
	while ((m = ml_dequeue(&ml)) != NULL) {	
		/* Prevent sending to closed socket */
		if (sc->sc_s == NULL || sc->sc_s->so_s == NULL) {
			sc->sc_ifp->if_oerrors++;
			m_freem(m);
			continue;
		}


		/* send packet */
		bytes = m->m_len;
		err = sosend(sc->sc_s->so_s, NULL, NULL, m, NULL, MSG_NOSIGNAL);
		if (err != 0) {
			sc->sc_ifp->if_oerrors++;
		} else
			sc->sc_ifp->if_obytes += bytes;
	}
}

/*
 * Receive a response on a socket.
 */
void
eourecv(struct socket *so, caddr_t upcallarg, int waitflag)
{
	struct eou_sock		*eso = (struct eou_sock *)upcallarg;
	struct eou_softc 	*ent, *sc = NULL;
	struct uio	 	 auio;
	struct iovec		 iov;
	struct eou_header	*pkt;
	struct eou_pingpong	*pp;
	struct mbuf		*data;
	struct mbuf_list	 data_l;
	uint64_t		 truet, utime;
	uint8_t			 truemac[SIPHASH_DIGEST_LENGTH];
	uint8_t			 msg[EOU_INTERNAL_MTU];
	int s, bytes, error = 0;
	int flag = (waitflag == M_DONTWAIT) ? MSG_DONTWAIT : 0;
	
	/* prevent receiving on a deleted socket */
	if (so == NULL || eso->so_s != so ||
	    (so->so_state & SS_ISCONNECTED) == 0)
		return;

	/* time of arrival */
	truet = (uint64_t)time_second;
	
	/* setup uio */
	bzero(&auio, sizeof(auio));
	bzero(&iov, sizeof(iov));
	bzero(msg, EOU_INTERNAL_MTU);
	iov.iov_base = &msg;
	iov.iov_len = EOU_INTERNAL_MTU;
	auio.uio_iov = &iov;
	auio.uio_iovcnt = 1;
	auio.uio_resid = EOU_INTERNAL_MTU;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;

	/* get packet */
	error = soreceive(so, NULL, &auio, NULL, NULL, &flag, 0);
	if (error != 0)
		goto err;
	bytes = EOU_INTERNAL_MTU - auio.uio_resid;

	/* parse header */
	if (bytes < sizeof(struct eou_header))
		goto err;
	pkt = (struct eou_header *)msg;

	/* get sc for network */
	SLIST_FOREACH(ent, &eou_sclist, sc_next) {
		if (ent->sc_s != NULL && ent->sc_s == eso &&
		    ntohl(pkt->eou_network) == ent->sc_network)
			sc = ent;
	}
	if (sc == NULL)
		goto err;
	sc->sc_ifp->if_ipackets++;

	/* handle packet */
	if (ntohs(pkt->eou_type) == EOU_T_PONG) {
		/* try to parse as a pong */
		if (bytes < sizeof(struct eou_pingpong))
			goto err;
		pp = (struct eou_pingpong *)msg;

		/* check time */
		utime = betoh64(pp->utime);
		if (utime > truet + 30 || utime < truet - 30)
			goto err;

		/* check mac */
		eou_gen_mac(pp, truemac);
		if (memcmp(truemac, pp->mac, sizeof(truemac)) != 0)
			goto err;

		/* valid packet! set link ready */
		sc->sc_gotpong = 1;
		timeout_add_sec(&sc->sc_pongtmo, EOU_PONG_TIMEOUT);
	} else if (ntohs(pkt->eou_type) == EOU_T_DATA && sc->sc_gotpong != 0) {
		/* strip and convert to mbuf */
		MGETHDR(data, M_NOWAIT, MT_DATA);
		if (data == NULL)
			goto err;
		data->m_len = 0;
		data->m_pkthdr.len = 0;
		if (m_copyback(data, 0, bytes - sizeof(struct eou_header),
		    msg + sizeof(struct eou_header), M_NOWAIT) != 0)
			goto err;

		/* add to mbuf list and pow */
		ml_init(&data_l);
		ml_enqueue(&data_l, data);

		s = splnet();
		if_input(sc->sc_ifp, &data_l);
		splx(s);
	} else
		goto err; /* Unknown/unhandled type */
	/* done */
	return;

err:
	if (sc != NULL)
		sc->sc_ifp->if_ierrors++;
}


/* MEDIA COMMANDS */	
int
eou_media_change(struct ifnet *ifp)
{
	return (0);
}

void
eou_media_status(struct ifnet *ifp, struct ifmediareq *imr)
{
	struct eou_softc *sc = (struct eou_softc *)ifp->if_softc;

	/* Assume link up if socket exists and pong received */
	if (sc->sc_s != NULL && sc->sc_gotpong)
		imr->ifm_status = IFM_AVALID | IFM_ACTIVE;
	else
		imr->ifm_status = IFM_AVALID;
}

/* HELPER COMMANDS */	
/*
 * Set up the source and destination addresses as given, as well as the port.
 * Updates the socket as neccessary to match this information.
 * Must be called within splnet.
 */
int
eou_set_address(struct ifnet *ifp, struct sockaddr_in *src,
    struct sockaddr_in *dst)
{
	struct eou_softc	*sc = (struct eou_softc *)ifp->if_softc;
	struct eou_sock		*new;
	int error = 0;

	if (src != NULL && dst != NULL) { /* setting new config */
		/* check addreses */
		if (src->sin_family != AF_INET || dst->sin_family != AF_INET)
			return (EAFNOSUPPORT); /* only support ipv4 */
		if (src->sin_len != sizeof(struct sockaddr_in) ||
		    dst->sin_len != sizeof(struct sockaddr_in))
			return (EINVAL);

		/* ensure relevant port */
		if (!dst->sin_port)
			dst->sin_port = htons(EOU_PORT);

		/* create a new socket to match */
		error = eou_sock_create(sc, src, dst, &new);
		if (error != 0)
			goto end;
		
		/* got socket; now try to delete previous one */
		if (sc->sc_s != NULL)
			eou_sock_delete(sc);
		
		/* update current info to match */
		sc->sc_s = new;
		
		/* start ping/pong messages */
		sc->sc_gotpong = 0;
		timeout_add_sec(&sc->sc_pingtmo, 1);
	} else { /* just delete old config */
		/* remove old timeouts */
		timeout_del(&sc->sc_pingtmo);
		timeout_del(&sc->sc_pongtmo);
		sc->sc_gotpong = 0;
		
		/* delete socket if present */
		if (sc->sc_s != NULL)
			eou_sock_delete(sc);
	}

end:
	return error;
}

/*
 * Creates and binds a new socket to match the addresses given.
 * Must be called within splnet.
 */
int
eou_sock_create(struct eou_softc *cur, struct sockaddr_in *src,
    struct sockaddr_in *dst, struct eou_sock **new)
{
	struct eou_softc	*scent;
	struct eou_sock		*ent, *so = NULL;
	struct socket		*s = NULL;
	struct sockaddr_in	*sa;
	struct mbuf		*m;
	int error = 0;

	/* Determine if socket already exists */
	SLIST_FOREACH(ent, &eou_socklist, so_next) {
		if (memcmp(src, &ent->so_src, sizeof(*src)) == 0 &&
		    memcmp(dst, &ent->so_dst, sizeof(*dst)) == 0) {
			so = ent;
			break;
		}
	}

	/* If it does, then return it or error if network already exists */
	if (so != NULL) {
		/* check if vnet exists for this socket */
		SLIST_FOREACH(scent, &eou_sclist, sc_next) {
			if (scent != cur && scent->sc_s == so &&
			    cur->sc_network == scent->sc_network) {
				return (EINVAL);
			}
		}

		*new = so;
		return (0);
	}

	/* Otherwise, create socket contents */
	if ((so = malloc(sizeof(*so), M_DEVBUF, M_NOWAIT|M_ZERO)) == NULL)
		return (ENOMEM);

	/* Create socket */
	error = socreate(AF_INET, &s, SOCK_DGRAM, 0);
	if (error)
		goto err;

	/* Bind */
	MGET(m, M_NOWAIT, MT_SONAME);
	if (m == NULL)
		goto err;
	m->m_len = src->sin_len;
	sa = mtod(m, struct sockaddr_in *);
	memcpy(sa, src, src->sin_len);

	error = sobind(s, m, curproc);
	m_freem(m);
	if (error) 
		goto err;

	/* Connect */
	MGETHDR(m, M_NOWAIT, MT_SONAME);
	if (m == NULL)
		goto err;
	m->m_len = 0;
	m->m_pkthdr.len = 0;
	error = m_copyback(m, 0, dst->sin_len, dst, M_NOWAIT);
	if (error) 
		goto err;

	error = soconnect(s, m);
	if (error)
		goto err;

	/* Socket struct settings */
	s->so_upcallarg = (caddr_t)so;
	s->so_upcall = eourecv;
	so->so_s = s;
	bzero(&so->so_src, sizeof(so->so_src));
	bzero(&so->so_dst, sizeof(so->so_dst));
	memcpy(&so->so_src, src, sizeof(*src));
	memcpy(&so->so_dst, dst, sizeof(*dst));
	*new = so;
	SLIST_INSERT_HEAD(&eou_socklist, so, so_next);

	return (0);
	/* NOTREACHED */
err:
	if (so != NULL)
		free(so, M_DEVBUF, sizeof(*so));
	if (s != NULL)
		soclose(s);
	if (error == 0)
		return (ENOBUFS);
	return (error);
}

/*
 * Delete and free the socket for this ifp matching the given addresses, or
 * simply set to null if it is currently referenced by other things.
 * Must be called within splnet.
 */
int
eou_sock_delete(struct eou_softc *cur)
{
	struct eou_softc *ent;
	struct eou_sock *so;

	/* dereference from cur */
	so = cur->sc_s;
	cur->sc_s = NULL;

	/* see if anything else references this */
	SLIST_FOREACH(ent, &eou_sclist, sc_next) {
		if (ent->sc_s == so)
			return (0);
	}

	/* nothing does! delete */
	so->so_s->so_upcall = NULL;
	so->so_s->so_upcallarg = NULL;

	/* close socket */
	soclose(so->so_s);
	SLIST_REMOVE(&eou_socklist, so, eou_sock, so_next);
	free(so, M_DEVBUF, sizeof(*so));

	return (0);
}

/*
 * Timer fires for sending a ping every 30 seconds.
 */
void
eou_timeout_ping(void *arg)
{
	struct eou_softc	*sc = arg;
	
	/* do nothing if socket is no more */
	if (sc->sc_s == NULL)
		return;

	/* add ping task */
	task_add(systq, &sc->sc_pingt);

	/* re-add timeout for next message */
	timeout_add_sec(&sc->sc_pingtmo, EOU_PING_TIMEOUT);
}

/*
 * Timer fires when we fail to receive a server pong for 100 seconds
 */
void
eou_timeout_pong(void *arg)
{
	struct eou_softc	*sc = arg;
	
	/* No pong message received - we simply say we haven't got one */
	sc->sc_gotpong = 0;
}

/*
 * Generates a mac for the given ping/pong message, placing it in the mac
 * pointer given.
 */
void
eou_gen_mac(struct eou_pingpong *msg, uint8_t *mac)
{
	SIPHASH_CTX ctx;
	uint8_t keybytes[SIPHASH_KEY_LENGTH] = EOU_KEY;

	SipHash24_Init(&ctx, (SIPHASH_KEY *)keybytes);
	SipHash24_Update(&ctx, &msg->hdr.eou_network,
	    sizeof(msg->hdr.eou_network));
	SipHash24_Update(&ctx, &msg->utime, sizeof(msg->utime));
	SipHash24_Update(&ctx, msg->random, sizeof(msg->random));
	SipHash24_Final(mac, &ctx);
}
