/*
 * lxc_native.c: LXC native configuration import
 *
 * Copyright (c) 2014-2016 Red Hat, Inc.
 * Copyright (c) 2013-2015 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "internal.h"
#include "lxc_container.h"
#include "lxc_native.h"
#include "util/viralloc.h"
#include "util/virfile.h"
#include "util/virlog.h"
#include "util/virstring.h"
#include "util/virconf.h"
#include "conf/domain_conf.h"
#include "virutil.h"

#define VIR_FROM_THIS VIR_FROM_LXC

VIR_LOG_INIT("lxc.lxc_native");

VIR_ENUM_IMPL(virLXCNetworkConfigEntry,
              VIR_LXC_NETWORK_CONFIG_LAST,
              "name",
              "type",
              "link",
              "hwaddr",
              "flags",
              "macvlan.mode",
              "vlan.id",
              "ipv4", /* Legacy: LXC IPv4 address */
              "ipv4.gateway",
              "ipv4.address",
              "ipv6", /* Legacy: LXC IPv6 address */
              "ipv6.gateway",
              "ipv6.address"
);

static virDomainFSDefPtr
lxcCreateFSDef(int type,
               const char *src,
               const char* dst,
               bool readonly,
               unsigned long long usage)
{
    virDomainFSDefPtr def;

    if (!(def = virDomainFSDefNew(NULL)))
        return NULL;

    def->type = type;
    def->accessmode = VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH;
    if (src)
        def->src->path = g_strdup(src);
    def->dst = g_strdup(dst);
    def->readonly = readonly;
    def->usage = usage;

    return def;
}

typedef struct _lxcFstab lxcFstab;
typedef lxcFstab *lxcFstabPtr;
struct _lxcFstab {
    lxcFstabPtr next;
    char *src;
    char *dst;
    char *type;
    char *options;
};

static void
lxcFstabFree(lxcFstabPtr fstab)
{
    while (fstab) {
        lxcFstabPtr next = NULL;
        next = fstab->next;

        g_free(fstab->src);
        g_free(fstab->dst);
        g_free(fstab->type);
        g_free(fstab->options);
        g_free(fstab);

        fstab = next;
    }
}

static char ** lxcStringSplit(const char *string)
{
    g_autofree char *tmp = NULL;
    size_t i;
    size_t ntokens = 0;
    char **parts;
    char **result = NULL;

    tmp = g_strdup(string);

    /* Replace potential \t by a space */
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '\t')
            tmp[i] = ' ';
    }

    if (!(parts = g_strsplit(tmp, " ", 0)))
        goto error;

    /* Append NULL element */
    VIR_EXPAND_N(result, ntokens, 1);

    for (i = 0; parts[i]; i++) {
        if (STREQ(parts[i], ""))
            continue;

        VIR_EXPAND_N(result, ntokens, 1);
        result[ntokens - 2] = g_strdup(parts[i]);
    }

    g_strfreev(parts);
    return result;

 error:
    g_strfreev(parts);
    g_strfreev(result);
    return NULL;
}

static lxcFstabPtr
lxcParseFstabLine(char *fstabLine)
{
    lxcFstabPtr fstab = NULL;
    char **parts;

    if (!fstabLine)
        return NULL;

    fstab = g_new0(lxcFstab, 1);
    if (!(parts = lxcStringSplit(fstabLine)))
        goto error;

    if (!parts[0] || !parts[1] || !parts[2] || !parts[3])
        goto error;

    fstab->src = g_strdup(parts[0]);
    fstab->dst = g_strdup(parts[1]);
    fstab->type = g_strdup(parts[2]);
    fstab->options = g_strdup(parts[3]);

    g_strfreev(parts);

    return fstab;

 error:
    lxcFstabFree(fstab);
    g_strfreev(parts);
    return NULL;
}

static int
lxcAddFSDef(virDomainDefPtr def,
            int type,
            const char *src,
            const char *dst,
            bool readonly,
            unsigned long long usage)
{
    virDomainFSDefPtr fsDef = NULL;

    if (!(fsDef = lxcCreateFSDef(type, src, dst, readonly, usage)))
        goto error;

    VIR_EXPAND_N(def->fss, def->nfss, 1);
    def->fss[def->nfss - 1] = fsDef;

    return 0;

 error:
    virDomainFSDefFree(fsDef);
    return -1;
}

static int
lxcSetRootfs(virDomainDefPtr def,
             virConfPtr properties)
{
    int type = VIR_DOMAIN_FS_TYPE_MOUNT;
    g_autofree char *value = NULL;

    if (virConfGetValueString(properties, "lxc.rootfs.path", &value) <= 0) {
        virResetLastError();

        /* Check for pre LXC 3.0 legacy key */
        if (virConfGetValueString(properties, "lxc.rootfs", &value) <= 0)
            return -1;
    }

    if (STRPREFIX(value, "/dev/"))
        type = VIR_DOMAIN_FS_TYPE_BLOCK;

    if (lxcAddFSDef(def, type, value, "/", false, 0) < 0)
        return -1;

    return 0;
}

static int
lxcConvertSize(const char *size, unsigned long long *value)
{
    char *unit = NULL;

    /* Split the string into value and unit */
    if (virStrToLong_ull(size, &unit, 10, value) < 0)
        goto error;

    if (STREQ(unit, "%")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("can't convert relative size: '%s'"),
                       size);
        return -1;
    } else {
        if (virScaleInteger(value, unit, 1, ULLONG_MAX) < 0)
            goto error;
    }

    return 0;

 error:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("failed to convert size: '%s'"),
                   size);
    return -1;
}

static int
lxcAddFstabLine(virDomainDefPtr def, lxcFstabPtr fstab)
{
    const char *src = NULL;
    g_autofree char *dst = NULL;
    char **options = g_strsplit(fstab->options, ",", 0);
    bool readonly;
    int type = VIR_DOMAIN_FS_TYPE_MOUNT;
    unsigned long long usage = 0;
    int ret = -1;

    if (!options)
        return -1;

    if (fstab->dst[0] != '/') {
        dst = g_strdup_printf("/%s", fstab->dst);
    } else {
        dst = g_strdup(fstab->dst);
    }

    /* Check that we don't add basic mounts */
    if (lxcIsBasicMountLocation(dst)) {
        ret = 0;
        goto cleanup;
    }

    if (STREQ(fstab->type, "tmpfs")) {
        char *sizeStr = NULL;
        size_t i;
        type = VIR_DOMAIN_FS_TYPE_RAM;

        for (i = 0; options[i]; i++) {
            if ((sizeStr = STRSKIP(options[i], "size="))) {
                if (lxcConvertSize(sizeStr, &usage) < 0)
                    goto cleanup;
                break;
            }
        }
        if (!sizeStr) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("missing tmpfs size, set the size option"));
            goto cleanup;
        }
    } else {
        src = fstab->src;
    }

    /* Is it a block device that needs special favor? */
    if (STRPREFIX(fstab->src, "/dev/"))
        type = VIR_DOMAIN_FS_TYPE_BLOCK;

    /* Do we have ro in options? */
    readonly = g_strv_contains((const char **)options, "ro");

    if (lxcAddFSDef(def, type, src, dst, readonly, usage) < 0)
        goto cleanup;

    ret = 1;

 cleanup:
    g_strfreev(options);
    return ret;
}

static int
lxcFstabWalkCallback(const char* name, virConfValuePtr value, void * data)
{
    int ret = 0;
    lxcFstabPtr fstabLine;
    virDomainDefPtr def = data;

    /* We only care about lxc.mount.entry lines */
    if (STRNEQ(name, "lxc.mount.entry"))
        return 0;

    fstabLine = lxcParseFstabLine(value->str);

    if (!fstabLine)
        return -1;

    if (lxcAddFstabLine(def, fstabLine) < 0)
        ret = -1;

    lxcFstabFree(fstabLine);
    return ret;
}

static virDomainNetDefPtr
lxcCreateNetDef(const char *type,
                const char *linkdev,
                const char *mac,
                const char *flag,
                const char *macvlanmode,
                const char *name)
{
    virDomainNetDefPtr net = NULL;
    virMacAddr macAddr;

    if (!(net = virDomainNetDefNew(NULL)))
        goto error;

    if (STREQ_NULLABLE(flag, "up"))
        net->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_UP;
    else
        net->linkstate = VIR_DOMAIN_NET_INTERFACE_LINK_STATE_DOWN;

    net->ifname_guest = g_strdup(name);

    if (mac && virMacAddrParse(mac, &macAddr) == 0)
        net->mac = macAddr;

    if (STREQ(type, "veth")) {
        if (linkdev) {
            net->type = VIR_DOMAIN_NET_TYPE_BRIDGE;
            net->data.bridge.brname = g_strdup(linkdev);
        } else {
            net->type = VIR_DOMAIN_NET_TYPE_ETHERNET;
        }
    } else if (STREQ(type, "macvlan")) {
        net->type = VIR_DOMAIN_NET_TYPE_DIRECT;

        if (!linkdev)
            goto error;

        net->data.direct.linkdev = g_strdup(linkdev);

        if (!macvlanmode || STREQ(macvlanmode, "private"))
            net->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_PRIVATE;
        else if (STREQ(macvlanmode, "vepa"))
            net->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_VEPA;
        else if (STREQ(macvlanmode, "bridge"))
            net->data.direct.mode = VIR_NETDEV_MACVLAN_MODE_BRIDGE;
        else
            VIR_WARN("Unknown macvlan type: %s", macvlanmode);
    }

    return net;

 error:
    virDomainNetDefFree(net);
    return NULL;
}

static virDomainHostdevDefPtr
lxcCreateHostdevDef(int mode, int type, const char *data)
{
    virDomainHostdevDefPtr hostdev = virDomainHostdevDefNew();

    if (!hostdev)
        return NULL;

    hostdev->mode = mode;
    hostdev->source.caps.type = type;

    if (type == VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET)
        hostdev->source.caps.u.net.ifname = g_strdup(data);

    return hostdev;
}

typedef struct _lxcNetworkParseData lxcNetworkParseData;
typedef lxcNetworkParseData *lxcNetworkParseDataPtr;
struct _lxcNetworkParseData {
    char *type;
    char *link;
    char *mac;
    char *flag;
    char *macvlanmode;
    char *vlanid;
    char *name;
    virNetDevIPAddrPtr *ips;
    size_t nips;
    char *gateway_ipv4;
    char *gateway_ipv6;
    size_t index;
};

typedef struct {
    size_t ndata;
    lxcNetworkParseDataPtr *parseData;
} lxcNetworkParseDataArray;


static int
lxcAddNetworkRouteDefinition(const char *address,
                             int family,
                             virNetDevIPRoutePtr **routes,
                             size_t *nroutes)
{
    g_autoptr(virNetDevIPRoute) route = NULL;
    g_autofree char *familyStr = NULL;
    g_autofree char *zero = NULL;

    zero = g_strdup(family == AF_INET ? VIR_SOCKET_ADDR_IPV4_ALL : VIR_SOCKET_ADDR_IPV6_ALL);

    familyStr = g_strdup(family == AF_INET ? "ipv4" : "ipv6");

    if (!(route = virNetDevIPRouteCreate(_("Domain interface"), familyStr,
                                         zero, NULL, address, 0, false,
                                         0, false)))
        return -1;

    if (VIR_APPEND_ELEMENT(*routes, *nroutes, route) < 0)
        return -1;

    return 0;
}

static int
lxcAddNetworkDefinition(virDomainDefPtr def, lxcNetworkParseData *data)
{
    virDomainNetDefPtr net = NULL;
    virDomainHostdevDefPtr hostdev = NULL;
    bool isPhys, isVlan = false;
    size_t i;

    if ((data->type == NULL) || STREQ(data->type, "empty") ||
         STREQ(data->type, "") ||  STREQ(data->type, "none"))
        return 0;

    isPhys = STREQ(data->type, "phys");
    isVlan = STREQ(data->type, "vlan");
    if (data->type != NULL && (isPhys || isVlan)) {
        if (!data->link) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("Missing 'link' attribute for NIC"));
            goto error;
        }
        if (!(hostdev = lxcCreateHostdevDef(VIR_DOMAIN_HOSTDEV_MODE_CAPABILITIES,
                                            VIR_DOMAIN_HOSTDEV_CAPS_TYPE_NET,
                                            data->link)))
            goto error;

        /* This still requires the user to manually setup the vlan interface
         * on the host */
        if (isVlan && data->vlanid) {
            g_free(hostdev->source.caps.u.net.ifname);
            hostdev->source.caps.u.net.ifname = g_strdup_printf("%s.%s",
                                                                data->link,
                                                                data->vlanid);
        }

        hostdev->source.caps.u.net.ip.ips = data->ips;
        hostdev->source.caps.u.net.ip.nips = data->nips;

        if (data->gateway_ipv4 &&
            lxcAddNetworkRouteDefinition(data->gateway_ipv4, AF_INET,
                                         &hostdev->source.caps.u.net.ip.routes,
                                         &hostdev->source.caps.u.net.ip.nroutes) < 0)
                goto error;

        if (data->gateway_ipv6 &&
            lxcAddNetworkRouteDefinition(data->gateway_ipv6, AF_INET6,
                                         &hostdev->source.caps.u.net.ip.routes,
                                         &hostdev->source.caps.u.net.ip.nroutes) < 0)
                goto error;

        VIR_EXPAND_N(def->hostdevs, def->nhostdevs, 1);
        def->hostdevs[def->nhostdevs - 1] = hostdev;
    } else {
        if (!(net = lxcCreateNetDef(data->type, data->link, data->mac,
                                    data->flag, data->macvlanmode,
                                    data->name)))
            goto error;

        net->guestIP.ips = data->ips;
        net->guestIP.nips = data->nips;

        if (data->gateway_ipv4 &&
            lxcAddNetworkRouteDefinition(data->gateway_ipv4, AF_INET,
                                         &net->guestIP.routes,
                                         &net->guestIP.nroutes) < 0)
                goto error;

        if (data->gateway_ipv6 &&
            lxcAddNetworkRouteDefinition(data->gateway_ipv6, AF_INET6,
                                         &net->guestIP.routes,
                                         &net->guestIP.nroutes) < 0)
                goto error;

        VIR_EXPAND_N(def->nets, def->nnets, 1);
        def->nets[def->nnets - 1] = net;
    }

    return 1;

 error:
    for (i = 0; i < data->nips; i++)
        g_free(data->ips[i]);
    g_free(data->ips);
    data->ips = NULL;
    virDomainNetDefFree(net);
    virDomainHostdevDefFree(hostdev);
    return -1;
}


static int
lxcNetworkParseDataIPs(const char *name,
                       virConfValuePtr value,
                       lxcNetworkParseData *parseData)
{
    int family = AF_INET;
    char **ipparts = NULL;
    g_autofree virNetDevIPAddrPtr ip = g_new0(virNetDevIPAddr, 1);

    if (STREQ(name, "ipv6") || STREQ(name, "ipv6.address"))
        family = AF_INET6;

    ipparts = g_strsplit(value->str, "/", 2);
    if (!ipparts || !ipparts[0] || !ipparts[1] ||
        virSocketAddrParse(&ip->address, ipparts[0], family) < 0 ||
        virStrToLong_ui(ipparts[1], NULL, 10, &ip->prefix) < 0) {

        virReportError(VIR_ERR_INVALID_ARG,
                       _("Invalid CIDR address: '%s'"), value->str);

        g_strfreev(ipparts);
        return -1;
    }

    g_strfreev(ipparts);

    if (VIR_APPEND_ELEMENT(parseData->ips, parseData->nips, ip) < 0)
        return -1;

    return 0;
}


static int
lxcNetworkParseDataSuffix(const char *entry,
                          virConfValuePtr value,
                          lxcNetworkParseData *parseData)
{
    int elem = virLXCNetworkConfigEntryTypeFromString(entry);

    switch (elem) {
    case VIR_LXC_NETWORK_CONFIG_TYPE:
        parseData->type = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_LINK:
        parseData->link = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_HWADDR:
        parseData->mac = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_FLAGS:
        parseData->flag = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_MACVLAN_MODE:
        parseData->macvlanmode = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_VLAN_ID:
        parseData->vlanid = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_NAME:
        parseData->name = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_IPV4:
    case VIR_LXC_NETWORK_CONFIG_IPV4_ADDRESS:
    case VIR_LXC_NETWORK_CONFIG_IPV6:
    case VIR_LXC_NETWORK_CONFIG_IPV6_ADDRESS:
        if (lxcNetworkParseDataIPs(entry, value, parseData) < 0)
            return -1;
        break;
    case VIR_LXC_NETWORK_CONFIG_IPV4_GATEWAY:
        parseData->gateway_ipv4 = value->str;
        break;
    case VIR_LXC_NETWORK_CONFIG_IPV6_GATEWAY:
        parseData->gateway_ipv6 = value->str;
        break;
    default:
        VIR_WARN("Unhandled network property: %s = %s",
                 entry,
                 value->str);
        return -1;
    }

    return 0;
}


static lxcNetworkParseDataPtr
lxcNetworkGetParseDataByIndex(lxcNetworkParseDataArray *networks,
                              unsigned int index)
{
    size_t ndata = networks->ndata;
    size_t i;

    for (i = 0; i < ndata; i++) {
        if (networks->parseData[i]->index == index)
            return networks->parseData[i];
    }

    /* Index was not found. So, it is time to add new *
     * interface and return this last position.       */
    VIR_EXPAND_N(networks->parseData, networks->ndata, 1);
    networks->parseData[ndata] = g_new0(lxcNetworkParseData, 1);
    networks->parseData[ndata]->index = index;

    return networks->parseData[ndata];
}


static int
lxcNetworkParseDataEntry(const char *name,
                         virConfValuePtr value,
                         lxcNetworkParseDataArray *networks)
{
    lxcNetworkParseData *parseData;
    const char *suffix_tmp = STRSKIP(name, "lxc.net.");
    char *suffix = NULL;
    unsigned long long index;

    if (virStrToLong_ull(suffix_tmp, &suffix, 10, &index) < 0)
        return -1;

    if (suffix[0] != '.')
        return -1;

    suffix++;

    if (!(parseData = lxcNetworkGetParseDataByIndex(networks, index)))
        return -1;

    return lxcNetworkParseDataSuffix(suffix, value, parseData);
}


static lxcNetworkParseDataPtr
lxcNetworkGetParseDataByIndexLegacy(lxcNetworkParseDataArray *networks,
                                    const char *entry)
{
    int elem = virLXCNetworkConfigEntryTypeFromString(entry);
    size_t ndata = networks->ndata;

    if (elem == VIR_LXC_NETWORK_CONFIG_TYPE) {
        /* Index was not found. So, it is time to add new *
         * interface and return this last position.       */
        VIR_EXPAND_N(networks->parseData, networks->ndata, 1);
        networks->parseData[ndata] = g_new0(lxcNetworkParseData, 1);
        networks->parseData[ndata]->index = networks->ndata;

        return networks->parseData[ndata];
    }

    /* Return last element added like a stack. */
    if (ndata > 0)
        return networks->parseData[ndata - 1];

    /* Not able to retrieve an element */
    return NULL;
}


static int
lxcNetworkParseDataEntryLegacy(const char *name,
                               virConfValuePtr value,
                               lxcNetworkParseDataArray *networks)
{
    const char *suffix = STRSKIP(name, "lxc.network.");
    lxcNetworkParseData *parseData;

    if (!(parseData = lxcNetworkGetParseDataByIndexLegacy(networks, suffix)))
        return -1;

    return lxcNetworkParseDataSuffix(suffix, value, parseData);
}


static int
lxcNetworkWalkCallback(const char *name, virConfValuePtr value, void *data)
{
    lxcNetworkParseDataArray *networks = data;

    if (STRPREFIX(name, "lxc.network."))
        return lxcNetworkParseDataEntryLegacy(name, value, networks);
    if (STRPREFIX(name, "lxc.net."))
        return lxcNetworkParseDataEntry(name, value, networks);

    return 0;
}

static int
lxcConvertNetworkSettings(virDomainDefPtr def, virConfPtr properties)
{
    int status;
    bool privnet = true;
    size_t i, j;
    lxcNetworkParseDataArray networks = {0, NULL};
    int ret = -1;

    networks.parseData = g_new0(lxcNetworkParseDataPtr, 1);

    if (virConfWalk(properties, lxcNetworkWalkCallback, &networks) < 0)
        goto error;

    for (i = 0; i < networks.ndata; i++) {
        lxcNetworkParseDataPtr data = networks.parseData[i];

        status = lxcAddNetworkDefinition(def, data);

        if (status < 0)
            goto error;
        else if (data->type != NULL && STREQ(data->type, "none"))
            privnet = false;
    }

    if (networks.ndata == 0 && privnet) {
        /* When no network type is provided LXC only adds loopback */
        def->features[VIR_DOMAIN_FEATURE_PRIVNET] = VIR_TRISTATE_SWITCH_ON;
    }

    ret = 0;

 cleanup:
    for (i = 0; i < networks.ndata; i++)
        g_free(networks.parseData[i]);
    g_free(networks.parseData);
    networks.parseData = NULL;
    return ret;

 error:
    for (i = 0; i < networks.ndata; i++) {
        lxcNetworkParseDataPtr data = networks.parseData[i];
        for (j = 0; j < data->nips; j++)
            g_free(data->ips[j]);
        g_free(data->ips);
        data->ips = NULL;
    }
    goto cleanup;
}

static int
lxcCreateConsoles(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;
    int nbttys = 0;
    virDomainChrDefPtr console;
    size_t i;

    if (virConfGetValueString(properties, "lxc.tty.max", &value) <= 0) {
        virResetLastError();

        /* Check for pre LXC 3.0 legacy key */
        if (virConfGetValueString(properties, "lxc.tty", &value) <= 0)
            return 0;
    }

    if (virStrToLong_i(value, NULL, 10, &nbttys) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("failed to parse int: '%s'"),
                       value);
        return -1;
    }

    def->consoles = g_new0(virDomainChrDefPtr, nbttys);

    def->nconsoles = nbttys;
    for (i = 0; i < nbttys; i++) {
        if (!(console = virDomainChrDefNew(NULL)))
            goto error;

        console->deviceType = VIR_DOMAIN_CHR_DEVICE_TYPE_CONSOLE;
        console->targetType = VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_LXC;
        console->target.port = i;
        console->source->type = VIR_DOMAIN_CHR_TYPE_PTY;

        def->consoles[i] = console;
    }

    return 0;

 error:
    virDomainChrDefFree(console);
    return -1;
}

static int
lxcIdmapWalkCallback(const char *name, virConfValuePtr value, void *data)
{
    virDomainDefPtr def = data;
    virDomainIdMapEntryPtr idmap = NULL;
    char type;
    unsigned long start, target, count;

    /* LXC 3.0 uses "lxc.idmap", while legacy used "lxc.id_map" */
    if (STRNEQ(name, "lxc.idmap") || !value->str) {
        if (!value->str || STRNEQ(name, "lxc.id_map"))
            return 0;
    }

    if (sscanf(value->str, "%c %lu %lu %lu", &type,
               &target, &start, &count) != 4) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("invalid %s: '%s'"),
                       name, value->str);
        return -1;
    }

    if (type == 'u') {
        VIR_EXPAND_N(def->idmap.uidmap, def->idmap.nuidmap, 1);
        idmap = &def->idmap.uidmap[def->idmap.nuidmap - 1];
    } else if (type == 'g') {
        VIR_EXPAND_N(def->idmap.gidmap, def->idmap.ngidmap, 1);
        idmap = &def->idmap.gidmap[def->idmap.ngidmap - 1];
    } else {
        return -1;
    }

    idmap->start = start;
    idmap->target = target;
    idmap->count = count;

    return 0;
}

static int
lxcSetMemTune(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;
    unsigned long long size = 0;

    if (virConfGetValueString(properties,
                              "lxc.cgroup.memory.limit_in_bytes",
                              &value) > 0) {
        if (lxcConvertSize(value, &size) < 0)
            return -1;
        size = size / 1024;
        virDomainDefSetMemoryTotal(def, size);
        def->mem.hard_limit = virMemoryLimitTruncate(size);
        g_free(value);
        value = NULL;
    }

    if (virConfGetValueString(properties,
                              "lxc.cgroup.memory.soft_limit_in_bytes",
                              &value) > 0) {
        if (lxcConvertSize(value, &size) < 0)
            return -1;
        def->mem.soft_limit = virMemoryLimitTruncate(size / 1024);
        g_free(value);
        value = NULL;
    }

    if (virConfGetValueString(properties,
                              "lxc.cgroup.memory.memsw.limit_in_bytes",
                              &value) > 0) {
        if (lxcConvertSize(value, &size) < 0)
            return -1;
        def->mem.swap_hard_limit = virMemoryLimitTruncate(size / 1024);
    }
    return 0;
}

static int
lxcSetCpuTune(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;

    if (virConfGetValueString(properties, "lxc.cgroup.cpu.shares",
                              &value) > 0) {
        if (virStrToLong_ull(value, NULL, 10, &def->cputune.shares) < 0)
            goto error;
        def->cputune.sharesSpecified = true;
        g_free(value);
        value = NULL;
    }

    if (virConfGetValueString(properties, "lxc.cgroup.cpu.cfs_quota_us",
                              &value) > 0) {
        if (virStrToLong_ll(value, NULL, 10, &def->cputune.quota) < 0)
            goto error;
        g_free(value);
        value = NULL;
    }

    if (virConfGetValueString(properties, "lxc.cgroup.cpu.cfs_period_us",
                              &value) > 0) {
        if (virStrToLong_ull(value, NULL, 10, &def->cputune.period) < 0)
            goto error;
    }

    return 0;

 error:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("failed to parse integer: '%s'"), value);
    return -1;
}

static int
lxcSetCpusetTune(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;
    virBitmapPtr nodeset = NULL;

    if (virConfGetValueString(properties, "lxc.cgroup.cpuset.cpus",
                              &value) > 0) {
        if (virBitmapParse(value, &def->cpumask, VIR_DOMAIN_CPUMASK_LEN) < 0)
            return -1;
        def->placement_mode = VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC;
        g_free(value);
        value = NULL;
    }

    if (virConfGetValueString(properties, "lxc.cgroup.cpuset.mems",
                              &value) > 0) {
        if (virBitmapParse(value, &nodeset, VIR_DOMAIN_CPUMASK_LEN) < 0)
            return -1;
        if (virDomainNumatuneSet(def->numa,
                                 def->placement_mode ==
                                 VIR_DOMAIN_CPU_PLACEMENT_MODE_STATIC,
                                 VIR_DOMAIN_NUMATUNE_PLACEMENT_STATIC,
                                 VIR_DOMAIN_NUMATUNE_MEM_STRICT,
                                 nodeset) < 0) {
            virBitmapFree(nodeset);
            return -1;
        }
        virBitmapFree(nodeset);
    }

    return 0;
}

static int
lxcBlkioDeviceWalkCallback(const char *name, virConfValuePtr value, void *data)
{
    char **parts = NULL;
    virBlkioDevicePtr device = NULL;
    virDomainDefPtr def = data;
    size_t i = 0;
    g_autofree char *path = NULL;
    int ret = -1;

    if (!STRPREFIX(name, "lxc.cgroup.blkio.") ||
            STREQ(name, "lxc.cgroup.blkio.weight")|| !value->str)
        return 0;

    if (!(parts = lxcStringSplit(value->str)))
        return -1;

    if (!parts[0] || !parts[1]) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("invalid %s value: '%s'"),
                       name, value->str);
        goto cleanup;
    }

    path = g_strdup_printf("/dev/block/%s", parts[0]);

    /* Do we already have a device definition for this path?
     * Get that device or create a new one */
    for (i = 0; !device && i < def->blkio.ndevices; i++) {
        if (STREQ(def->blkio.devices[i].path, path))
            device = &def->blkio.devices[i];
    }
    if (!device) {
        VIR_EXPAND_N(def->blkio.devices, def->blkio.ndevices, 1);
        device = &def->blkio.devices[def->blkio.ndevices - 1];
        device->path = g_steal_pointer(&path);
    }

    /* Set the value */
    if (STREQ(name, "lxc.cgroup.blkio.device_weight")) {
        if (virStrToLong_ui(parts[1], NULL, 10, &device->weight) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse device weight: '%s'"), parts[1]);
            goto cleanup;
        }
    } else if (STREQ(name, "lxc.cgroup.blkio.throttle.read_bps_device")) {
        if (virStrToLong_ull(parts[1], NULL, 10, &device->rbps) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse read_bps_device: '%s'"),
                           parts[1]);
            goto cleanup;
        }
    } else if (STREQ(name, "lxc.cgroup.blkio.throttle.write_bps_device")) {
        if (virStrToLong_ull(parts[1], NULL, 10, &device->wbps) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse write_bps_device: '%s'"),
                           parts[1]);
            goto cleanup;
        }
    } else if (STREQ(name, "lxc.cgroup.blkio.throttle.read_iops_device")) {
        if (virStrToLong_ui(parts[1], NULL, 10, &device->riops) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse read_iops_device: '%s'"),
                           parts[1]);
            goto cleanup;
        }
    } else if (STREQ(name, "lxc.cgroup.blkio.throttle.write_iops_device")) {
        if (virStrToLong_ui(parts[1], NULL, 10, &device->wiops) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse write_iops_device: '%s'"),
                           parts[1]);
            goto cleanup;
        }
    } else {
        VIR_WARN("Unhandled blkio tune config: %s", name);
    }

    ret = 0;

 cleanup:
    g_strfreev(parts);
    return ret;
}

static int
lxcSetBlkioTune(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;

    if (virConfGetValueString(properties, "lxc.cgroup.blkio.weight",
                              &value) > 0) {
        if (virStrToLong_ui(value, NULL, 10, &def->blkio.weight) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("failed to parse integer: '%s'"), value);
            return -1;
        }
    }

    if (virConfWalk(properties, lxcBlkioDeviceWalkCallback, def) < 0)
        return -1;

    return 0;
}

static void
lxcSetCapDrop(virDomainDefPtr def, virConfPtr properties)
{
    g_autofree char *value = NULL;
    char **toDrop = NULL;
    const char *capString;
    size_t i;

    if (virConfGetValueString(properties, "lxc.cap.drop", &value) > 0)
        toDrop = g_strsplit(value, " ", 0);

    for (i = 0; i < VIR_DOMAIN_PROCES_CAPS_FEATURE_LAST; i++) {
        capString = virDomainProcessCapsFeatureTypeToString(i);
        if (toDrop != NULL &&
            g_strv_contains((const char **)toDrop, capString))
            def->caps_features[i] = VIR_TRISTATE_SWITCH_OFF;
    }

    def->features[VIR_DOMAIN_FEATURE_CAPABILITIES] = VIR_DOMAIN_CAPABILITIES_POLICY_ALLOW;

    g_strfreev(toDrop);
}

virDomainDefPtr
lxcParseConfigString(const char *config,
                     virCapsPtr caps G_GNUC_UNUSED,
                     virDomainXMLOptionPtr xmlopt)
{
    virDomainDefPtr vmdef = NULL;
    g_autoptr(virConf) properties = NULL;
    g_autofree char *value = NULL;

    if (!(properties = virConfReadString(config, VIR_CONF_FLAG_LXC_FORMAT)))
        return NULL;

    if (!(vmdef = virDomainDefNew()))
        goto error;

    if (virUUIDGenerate(vmdef->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to generate uuid"));
        goto error;
    }

    vmdef->id = -1;
    virDomainDefSetMemoryTotal(vmdef, 64 * 1024);

    vmdef->onReboot = VIR_DOMAIN_LIFECYCLE_ACTION_RESTART;
    vmdef->onCrash = VIR_DOMAIN_LIFECYCLE_ACTION_DESTROY;
    vmdef->onPoweroff = VIR_DOMAIN_LIFECYCLE_ACTION_DESTROY;
    vmdef->virtType = VIR_DOMAIN_VIRT_LXC;

    /* Value not handled by the LXC driver, setting to
     * minimum required to make XML parsing pass */
    if (virDomainDefSetVcpusMax(vmdef, 1, xmlopt) < 0)
        goto error;

    if (virDomainDefSetVcpus(vmdef, 1) < 0)
        goto error;

    vmdef->nfss = 0;
    vmdef->os.type = VIR_DOMAIN_OSTYPE_EXE;

    if (virConfGetValueString(properties, "lxc.arch", &value) > 0) {
        virArch arch = virArchFromString(value);
        if (arch == VIR_ARCH_NONE && STREQ(value, "x86"))
            arch = VIR_ARCH_I686;
        else if (arch == VIR_ARCH_NONE && STREQ(value, "amd64"))
            arch = VIR_ARCH_X86_64;
        vmdef->os.arch = arch;
        g_free(value);
        value = NULL;
    }

    vmdef->os.init = g_strdup("/sbin/init");

    if (virConfGetValueString(properties, "lxc.uts.name", &value) <= 0) {
        virResetLastError();

        /* Check for pre LXC 3.0 legacy key */
        if (virConfGetValueString(properties, "lxc.utsname", &value) <= 0)
            goto error;
    }

    vmdef->name = g_strdup(value);

    if (!vmdef->name)
        vmdef->name = g_strdup("unnamed");

    if (lxcSetRootfs(vmdef, properties) < 0)
        goto error;

    /* LXC 3.0 uses "lxc.mount.fstab", while legacy used just "lxc.mount".
     * In either case, generate the error to use "lxc.mount.entry" instead */
    if (virConfGetValue(properties, "lxc.mount.fstab") ||
        virConfGetValue(properties, "lxc.mount")) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("lxc.mount.fstab or lxc.mount found, use "
                         "lxc.mount.entry lines instead"));
        goto error;
    }

    /* Loop over lxc.mount.entry to add filesystem devices for them */
    if (virConfWalk(properties, lxcFstabWalkCallback, vmdef) < 0)
        goto error;

    /* Network configuration */
    if (lxcConvertNetworkSettings(vmdef, properties) < 0)
        goto error;

    /* Consoles */
    if (lxcCreateConsoles(vmdef, properties) < 0)
        goto error;

    /* lxc.idmap or legacy lxc.id_map */
    if (virConfWalk(properties, lxcIdmapWalkCallback, vmdef) < 0)
        goto error;

    /* lxc.cgroup.memory.* */
    if (lxcSetMemTune(vmdef, properties) < 0)
        goto error;

    /* lxc.cgroup.cpu.* */
    if (lxcSetCpuTune(vmdef, properties) < 0)
        goto error;

    /* lxc.cgroup.cpuset.* */
    if (lxcSetCpusetTune(vmdef, properties) < 0)
        goto error;

    /* lxc.cgroup.blkio.* */
    if (lxcSetBlkioTune(vmdef, properties) < 0)
        goto error;

    /* lxc.cap.drop */
    lxcSetCapDrop(vmdef, properties);

    if (virDomainDefPostParse(vmdef, VIR_DOMAIN_DEF_PARSE_ABI_UPDATE,
                              xmlopt, NULL) < 0)
        goto error;

    return vmdef;

 error:
    virDomainDefFree(vmdef);
    return NULL;
}
