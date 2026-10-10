/*
 * test/fuzz/cmp_fuzz.c --
 *	libFuzzer target for the libdb comparator (src/sm/sm_cmp.c), which
 *	libdb calls on unvalidated page bytes: split the input into two keys
 *	and compare them both ways.  It must never read out of bounds
 *	(ASan), and must be antisymmetric.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dbsql_int.h"
#include "vdbe_int.h"
#include "sm_int.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static sm_dbh_t dbh;		/* no key_info: as the garbage test */
	DB fake;
	DBT a, b;
	u_int8_t *pa, *pb;
	size_t na, nb;
	int ab, ba;

	if (size < 1)
		return (0);
	na = data[0] % size;
	nb = size - 1 - na;
	/* Exact-size heap copies: ASan flags any read past either key. */
	pa = malloc(na ? na : 1);
	pb = malloc(nb ? nb : 1);
	memcpy(pa, data + 1, na);
	memcpy(pb, data + 1 + na, nb);
	memset(&fake, 0, sizeof(fake));
	fake.app_private = &dbh;
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	a.data = pa;
	a.size = (u_int32_t)na;
	b.data = pb;
	b.size = (u_int32_t)nb;
	ab = __sm_rec_cmp(&fake, &a, &b);
	ba = __sm_rec_cmp(&fake, &b, &a);
	if ((ab < 0) != (ba > 0) || (ab > 0) != (ba < 0))
		abort();		/* not antisymmetric */
	free(pa);
	free(pb);
	return (0);
}
