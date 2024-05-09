module net

#include <unistd.h>
#include <sys/select.h>
// inet.h is needed for inet_ntop on macos
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>

#flag solaris -lsocket

const is_windows = false

pub fn error_code() int {
	return C.errno
}

fn init() {
}

pub const msg_nosignal = 0x4000
pub const msg_dontwait = C.MSG_DONTWAIT

pub const error_ewouldblock = C.EWOULDBLOCK
pub const error_einprogress = C.EINPROGRESS
pub const error_eagain = C.EAGAIN

fn C.unlink(&char) int
