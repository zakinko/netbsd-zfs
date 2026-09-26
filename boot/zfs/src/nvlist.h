/*	$NetBSD$	*/

#ifndef _LIBSA_NVLIST_H_
#define	_LIBSA_NVLIST_H_

#define	NV_WANT_UINT64	1
#define	NV_WANT_STRING	2
#define	NV_WANT_NVLIST	3

struct nvpair_value {
	uint64_t	nv_u64;
	const char	*nv_string;	/* not NUL terminated */
	uint32_t	nv_strlen;
	const uint8_t	*nv_list;
	size_t		nv_listlen;
	uint32_t	nv_nelem;
};

int	nvlist_find(const void *, size_t, const char *, int,
	    struct nvpair_value *);
int	nvlist_find_nested(const void *, size_t, const char *, int,
	    struct nvpair_value *);
int	nvlist_array_elem(const struct nvpair_value *, uint32_t,
	    struct nvpair_value *);

#endif	/* _LIBSA_NVLIST_H_ */
