/*
 * linux/fs/nfs/namespace.c
 *
 * Copyright (C) 2005 Trond Myklebust <Trond.Myklebust@netapp.com>
 * - Modified by David Howells <dhowells@redhat.com>
 *
 * NFS namespace
 */

#include <linux/dcache.h>
#include <linux/gfp.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nfs_fs.h>
#include <linux/string.h>
#include <linux/sunrpc/clnt.h>
#include <linux/vfs.h>
#include <linux/sunrpc/gss_api.h>
#include "internal.h"

#define NFSDBG_FACILITY		NFSDBG_VFS

static void nfs_expire_automounts(struct work_struct *work);

static LIST_HEAD(nfs_automount_list);
static DECLARE_DELAYED_WORK(nfs_automount_task, nfs_expire_automounts);
int nfs_mountpoint_expiry_timeout = 500 * HZ;

static struct vfsmount *nfs_do_submount(struct dentry *dentry,
					struct nfs_fh *fh,
					struct nfs_fattr *fattr,
					rpc_authflavor_t authflavor);

char *nfs_path(char **p, struct dentry *dentry, char *buffer, ssize_t buflen)
{
	char *end;
	int namelen;
	unsigned seq;
	const char *base;

rename_retry:
	end = buffer+buflen;
	*--end = '\0';
	buflen--;

	seq = read_seqbegin(&rename_lock);
	rcu_read_lock();
	while (1) {
		spin_lock(&dentry->d_lock);
		if (IS_ROOT(dentry))
			break;
		namelen = dentry->d_name.len;
		buflen -= namelen + 1;
		if (buflen < 0)
			goto Elong_unlock;
		end -= namelen;
		memcpy(end, dentry->d_name.name, namelen);
		*--end = '/';
		spin_unlock(&dentry->d_lock);
		dentry = dentry->d_parent;
	}
	if (read_seqretry(&rename_lock, seq)) {
		spin_unlock(&dentry->d_lock);
		rcu_read_unlock();
		goto rename_retry;
	}
	if ((flags & NFS_PATH_CANONICAL) && *end != '/') {
		if (--buflen < 0) {
			spin_unlock(&dentry->d_lock);
			rcu_read_unlock();
			goto Elong;
		}
		*--end = '/';
	}
	*p = end;
	base = dentry->d_fsdata;
	if (!base) {
		spin_unlock(&dentry->d_lock);
		rcu_read_unlock();
		WARN_ON(1);
		return end;
	}
	namelen = strlen(base);
	
	while (namelen > 0 && base[namelen - 1] == '/')
		namelen--;
	buflen -= namelen;
	if (buflen < 0) {
		spin_unlock(&dentry->d_lock);
		rcu_read_unlock();
		goto Elong;
	}
	end -= namelen;
	memcpy(end, base, namelen);
	spin_unlock(&dentry->d_lock);
	rcu_read_unlock();
	return end;
Elong_unlock:
	spin_unlock(&dentry->d_lock);
	rcu_read_unlock();
	if (read_seqretry(&rename_lock, seq))
		goto rename_retry;
Elong:
	return ERR_PTR(-ENAMETOOLONG);
}

#ifdef CONFIG_NFS_V4
rpc_authflavor_t nfs_find_best_sec(struct nfs4_secinfo_flavors *flavors)
{
	struct gss_api_mech *mech;
	struct xdr_netobj oid;
	int i;
	rpc_authflavor_t pseudoflavor = RPC_AUTH_UNIX;

	for (i = 0; i < flavors->num_flavors; i++) {
		struct nfs4_secinfo_flavor *flavor;
		flavor = &flavors->flavors[i];

		if (flavor->flavor == RPC_AUTH_NULL || flavor->flavor == RPC_AUTH_UNIX) {
			pseudoflavor = flavor->flavor;
			break;
		} else if (flavor->flavor == RPC_AUTH_GSS) {
			oid.len  = flavor->gss.sec_oid4.len;
			oid.data = flavor->gss.sec_oid4.data;
			mech = gss_mech_get_by_OID(&oid);
			if (!mech)
				continue;
			pseudoflavor = gss_svc_to_pseudoflavor(mech, flavor->gss.service);
			gss_mech_put(mech);
			break;
		}
	}

	return pseudoflavor;
}

static struct rpc_clnt *nfs_lookup_mountpoint(struct inode *dir,
					      struct qstr *name,
					      struct nfs_fh *fh,
					      struct nfs_fattr *fattr)
{
	int err;

	if (NFS_PROTO(dir)->version == 4)
		return nfs4_proc_lookup_mountpoint(dir, name, fh, fattr);

	err = NFS_PROTO(dir)->lookup(NFS_SERVER(dir)->client, dir, name, fh, fattr);
	if (err)
		return ERR_PTR(err);
	return rpc_clone_client(NFS_SERVER(dir)->client);
}
#else 
static inline struct rpc_clnt *nfs_lookup_mountpoint(struct inode *dir,
						     struct qstr *name,
						     struct nfs_fh *fh,
						     struct nfs_fattr *fattr)
{
	int err = NFS_PROTO(dir)->lookup(NFS_SERVER(dir)->client, dir, name, fh, fattr);
	if (err)
		return ERR_PTR(err);
	return rpc_clone_client(NFS_SERVER(dir)->client);
}
#endif 

struct vfsmount *nfs_d_automount(struct path *path)
{
	struct vfsmount *mnt;
	struct dentry *parent;
	struct nfs_fh *fh = NULL;
	struct nfs_fattr *fattr = NULL;
	struct rpc_clnt *client;

	dprintk("--> nfs_d_automount()\n");

	mnt = ERR_PTR(-ESTALE);
	if (IS_ROOT(path->dentry))
		goto out_nofree;

	mnt = ERR_PTR(-ENOMEM);
	fh = nfs_alloc_fhandle();
	fattr = nfs_alloc_fattr();
	if (fh == NULL || fattr == NULL)
		goto out;

	dprintk("%s: enter\n", __func__);

	
	parent = dget_parent(path->dentry);
	client = nfs_lookup_mountpoint(parent->d_inode, &path->dentry->d_name, fh, fattr);
	dput(parent);
	if (IS_ERR(client)) {
		mnt = ERR_CAST(client);
		goto out;
	}

	if (fattr->valid & NFS_ATTR_FATTR_V4_REFERRAL)
		mnt = nfs_do_refmount(client, path->dentry);
	else
		mnt = nfs_do_submount(path->dentry, fh, fattr, client->cl_auth->au_flavor);
	rpc_shutdown_client(client);

	if (IS_ERR(mnt))
		goto out;

	dprintk("%s: done, success\n", __func__);
	mntget(mnt); 
	mnt_set_expiry(mnt, &nfs_automount_list);
	schedule_delayed_work(&nfs_automount_task, nfs_mountpoint_expiry_timeout);

out:
	nfs_free_fattr(fattr);
	nfs_free_fhandle(fh);
out_nofree:
	if (IS_ERR(mnt))
		dprintk("<-- %s(): error %ld\n", __func__, PTR_ERR(mnt));
	else
		dprintk("<-- %s() = %p\n", __func__, mnt);
	return mnt;
}

const struct inode_operations nfs_mountpoint_inode_operations = {
	.getattr	= nfs_getattr,
};

const struct inode_operations nfs_referral_inode_operations = {
};

static void nfs_expire_automounts(struct work_struct *work)
{
	struct list_head *list = &nfs_automount_list;

	mark_mounts_for_expiry(list);
	if (!list_empty(list))
		schedule_delayed_work(&nfs_automount_task, nfs_mountpoint_expiry_timeout);
}

void nfs_release_automount_timer(void)
{
	if (list_empty(&nfs_automount_list))
		cancel_delayed_work(&nfs_automount_task);
}

static struct vfsmount *nfs_do_clone_mount(struct nfs_server *server,
					   const char *devname,
					   struct nfs_clone_mount *mountdata)
{
#ifdef CONFIG_NFS_V4
	struct vfsmount *mnt = ERR_PTR(-EINVAL);
	switch (server->nfs_client->rpc_ops->version) {
		case 2:
		case 3:
			mnt = vfs_kern_mount(&nfs_xdev_fs_type, 0, devname, mountdata);
			break;
		case 4:
			mnt = vfs_kern_mount(&nfs4_xdev_fs_type, 0, devname, mountdata);
	}
	return mnt;
#else
	return vfs_kern_mount(&nfs_xdev_fs_type, 0, devname, mountdata);
#endif
}

static struct vfsmount *nfs_do_submount(struct dentry *dentry,
					struct nfs_fh *fh,
					struct nfs_fattr *fattr,
					rpc_authflavor_t authflavor)
{
	struct nfs_clone_mount mountdata = {
		.sb = dentry->d_sb,
		.dentry = dentry,
		.fh = fh,
		.fattr = fattr,
		.authflavor = authflavor,
	};
	struct vfsmount *mnt = ERR_PTR(-ENOMEM);
	char *page = (char *) __get_free_page(GFP_USER);
	char *devname;

	dprintk("--> nfs_do_submount()\n");

	dprintk("%s: submounting on %s/%s\n", __func__,
			dentry->d_parent->d_name.name,
			dentry->d_name.name);
	if (page == NULL)
		goto out;
	devname = nfs_devname(dentry, page, PAGE_SIZE);
	mnt = (struct vfsmount *)devname;
	if (IS_ERR(devname))
		goto free_page;
	mnt = nfs_do_clone_mount(NFS_SB(dentry->d_sb), devname, &mountdata);
free_page:
	free_page((unsigned long)page);
out:
	dprintk("%s: done\n", __func__);

	dprintk("<-- nfs_do_submount() = %p\n", mnt);
	return mnt;
}
