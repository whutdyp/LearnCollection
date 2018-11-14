/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/kernel_types.h>
#include <sys/sysproto.h>

#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/persona.h>
#include <sys/proc.h>

#include <libkern/libkern.h>

static int kpersona_copyin(user_addr_t infop, struct kpersona_info *kinfo)
{
	uint32_t info_v = 0;
	int error;

	error = copyin(infop, &info_v, sizeof(info_v));
	if (error)
		return error;

	/* only support a single version of the struct for now */
	if (info_v != PERSONA_INFO_V1)
		return EINVAL;

	error = copyin(infop, kinfo, sizeof(*kinfo));

	/* enforce NULL termination on strings */
	kinfo->persona_name[MAXLOGNAME] = 0;

	return error;
}

static int kpersona_copyout(struct kpersona_info *kinfo, user_addr_t infop)
{
	uint32_t info_v;
	int error;

	error = copyin(infop, &info_v, sizeof(info_v));
	if (error)
		return error;

	/* only support a single version of the struct for now */
	/* TODO: in the future compare info_v to kinfo->persona_info_version */
	if (info_v != PERSONA_INFO_V1)
		return EINVAL;

	error = copyout(kinfo, infop, sizeof(*kinfo));
	return error;
}


static int kpersona_alloc_syscall(user_addr_t infop, user_addr_t idp)
{
	int error;
	struct kpersona_info kinfo;
	struct persona *persona;
	uid_t id = PERSONA_ID_NONE;
	const char *login;

	/*
	 * TODO: rdar://problem/19981151
	 * Add entitlement check!
	 */
	if (!kauth_cred_issuser(kauth_cred_get()))
		return EPERM;

	error = kpersona_copyin(infop, &kinfo);
	if (error)
		return error;

	login = kinfo.persona_name[0] ? kinfo.persona_name : NULL;
	if (kinfo.persona_id != PERSONA_ID_NONE && kinfo.persona_id != (uid_t)0)
		id = kinfo.persona_id;

	error = 0;
	persona = persona_alloc(id, login, kinfo.persona_type, &error);
	if (!persona)
		return error;

	if (kinfo.persona_gid) {
		error = persona_set_gid(persona, kinfo.persona_gid);
		if (error)
			goto out_error;
	}

	if (kinfo.persona_ngroups > 0) {
		/* force gmuid 0 to *opt-out* of memberd */
		if (kinfo.persona_gmuid == 0)
			kinfo.persona_gmuid = KAUTH_UID_NONE;

		error = persona_set_groups(persona, kinfo.persona_groups,
					   kinfo.persona_ngroups,
					   kinfo.persona_gmuid);
		if (error)
			goto out_error;
	}

	error = copyout(&persona->pna_id, idp, sizeof(persona->pna_id));
	if (error)
		goto out_error;
	error = kpersona_copyout(&kinfo, infop);

	/*
	 * On success, we have a persona structure in the global list with a
	 * single reference count on it. The corresponding _dealloc() call
	 * will release this reference.
	 */
	return error;

out_error:
	printf("%s:  ERROR:%d\n", __func__, error);
	if (persona)
		persona_put(persona);
	return error;
}

static int kpersona_dealloc_syscall(user_addr_t idp)
{
	int error = 0;
	uid_t persona_id;
	struct persona *persona;

	if (!kauth_cred_issuser(kauth_cred_get()))
		return EPERM;

	error = copyin(idp, &persona_id, sizeof(persona_id));
	if (error)
		return error;

	/* invalidate the persona (deny subsequent spawn/fork) */
	persona = persona_lookup_and_invalidate(persona_id);

	if (!persona)
		return ESRCH;

	/* one reference from the _lookup() */
	persona_put(persona);

	/* one reference from the _alloc() */
	persona_put(persona);

	return error;
}

static int kpersona_get_syscall(user_addr_t idp)
{
	int error;
	struct persona *persona = current_persona_get();

	if (!persona)
		return ESRCH;

	error = copyout(&persona->pna_id, idp, sizeof(persona->pna_id));
	persona_put(persona);

	return error;
}

static int kpersona_info_syscall(user_addr_t idp, user_addr_t infop)
{
	int error;
	uid_t persona_id;
	struct persona *persona;
	struct kpersona_info kinfo;

	error = copyin(idp, &persona_id, sizeof(persona_id));
	if (error)
		return error;

	/*
	 * TODO: rdar://problem/19981151
	 * Add entitlement check!
	 */

	persona = persona_lookup(persona_id);
	if (!persona)
		return ESRCH;

	persona_dbg("FOUND: persona:%p, id:%d, gid:%d, login:\"%s\"",
		    persona, persona->pna_id, persona_get_gid(persona),
		    persona->pna_login);

	memset(&kinfo, 0, sizeof(kinfo));
	kinfo.persona_info_version = PERSONA_INFO_V1;
	kinfo.persona_id = persona->pna_id;
	kinfo.persona_type = persona->pna_type;
	kinfo.persona_gid = persona_get_gid(persona);
	unsigned ngroups = 0;
	persona_get_groups(persona, &ngroups, kinfo.persona_groups, NGROUPS);
	kinfo.persona_ngroups = ngroups;
	kinfo.persona_gmuid = persona_get_gmuid(persona);

	/*
	 * NULL termination is assured b/c persona_name is
	 * exactly MAXLOGNAME + 1 bytes (and has been memset to 0)
	 */
	strncpy(kinfo.persona_name, persona->pna_login, MAXLOGNAME);

	persona_put(persona);

	error = kpersona_copyout(&kinfo, infop);

	return error;
}

static int kpersona_pidinfo_syscall(user_addr_t idp, user_addr_t infop)
{
	int error;
	pid_t pid;
	struct persona *persona;
	struct kpersona_info kinfo;

	error = copyin(idp, &pid, sizeof(pid));
	if (error)
		return error;

	if (!kauth_cred_issuser(kauth_cred_get())
	    && (pid != current_proc()->p_pid))
		return EPERM;

	persona = persona_proc_get(pid);
	if (!persona)
		return ESRCH;

	memset(&kinfo, 0, sizeof(kinfo));
	kinfo.persona_info_version = PERSONA_INFO_V1;
	kinfo.persona_id = persona->pna_id;
	kinfo.persona_type = persona->pna_type;
	kinfo.persona_gid = persona_get_gid(persona);
	unsigned ngroups = 0;
	persona_get_groups(persona, &ngroups, kinfo.persona_groups, NGROUPS);
	kinfo.persona_ngroups = ngroups;
	kinfo.persona_gmuid = persona_get_gmuid(persona);

	strncpy(kinfo.persona_name, persona->pna_login, MAXLOGNAME);

	persona_put(persona);

	error = kpersona_copyout(&kinfo, infop);

	return error;
}

static int kpersona_find_syscall(user_addr_t infop, user_addr_t idp, user_addr_t idlenp)
{
	int error;
	struct kpersona_info kinfo;
	const char *login;
	size_t u_idlen, k_idlen = 0;
	struct persona **persona = NULL;

	error = copyin(idlenp, &u_idlen, sizeof(u_idlen));
	if (error)
		return error;

	if (u_idlen > g_max_personas)
		u_idlen = g_max_personas;

	error = kpersona_copyin(infop, &kinfo);
	if (error)
		goto out;

	login = kinfo.persona_name[0] ? kinfo.persona_name : NULL;

	if (u_idlen > 0) {
		MALLOC(persona, struct persona **, sizeof(*persona) * u_idlen,
		       M_TEMP, M_WAITOK|M_ZERO);
		if (!persona) {
			error = ENOMEM;
			goto out;
		}
	}

	k_idlen = u_idlen;
	error = persona_find(login, kinfo.persona_id, persona, &k_idlen);
	if (error)
		goto out;

	/* copyout all the IDs of each persona we found */
	for (size_t i = 0; i < k_idlen; i++) {
		if (i >= u_idlen)
			break;
		error = copyout(&persona[i]->pna_id,
				idp + (i * sizeof(persona[i]->pna_id)),
				sizeof(persona[i]->pna_id));
		if (error)
			goto out;
	}

out:
	if (persona) {
		for (size_t i = 0; i < u_idlen; i++)
			persona_put(persona[i]);
		FREE(persona, M_TEMP);
	}

	(void)copyout(&k_idlen, idlenp, sizeof(u_idlen));

	return error;
}


/*
 * Syscall entry point / demux.
 */
int persona(__unused proc_t p, struct persona_args *pargs, __unused int32_t *retval)
{
	int error;
	uint32_t op = pargs->operation;
	/* uint32_t flags = pargs->flags; */
	user_addr_t infop = pargs->info;
	user_addr_t idp = pargs->id;

	switch (op) {
	case PERSONA_OP_ALLOC:
		error = kpersona_alloc_syscall(infop, idp);
		break;
	case PERSONA_OP_DEALLOC:
		error = kpersona_dealloc_syscall(idp);
		break;
	case PERSONA_OP_GET:
		error = kpersona_get_syscall(idp);
		break;
	case PERSONA_OP_INFO:
		error = kpersona_info_syscall(idp, infop);
		break;
	case PERSONA_OP_PIDINFO:
		error = kpersona_pidinfo_syscall(idp, infop);
		break;
	case PERSONA_OP_FIND:
		error = kpersona_find_syscall(infop, idp, pargs->idlen);
		break;
	default:
		error = ENOSYS;
		break;
	}

	return error;
}