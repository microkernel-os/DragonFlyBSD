/*
 * dhcpcd config.h for dragonfly
 *
 * generated by 'configure', with custom changes
 */

#define	HAVE_SYS_QUEUE_H
#define	TAILQ_FOREACH_SAFE	TAILQ_FOREACH_MUTABLE
#define	HAVE_KQUEUE
#define	HAVE_REALLOCARRAY
#include			"compat/pidfile.h"
#include			"compat/strtoi.h"
#define	HAVE_MD5_H
#define	SHA2_H			<openssl/sha.h>
#include			"compat/crypt/hmac.h"
