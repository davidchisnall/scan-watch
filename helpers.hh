#pragma once
#include <errno.h>
#include <optional>
#include <string>
#include <sys/capsicum.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#include <variant>

namespace
{
	/**
	 * Simple RAII wrapper around a file descriptor.
	 */
	class FileDescriptor
	{
		/**
		 * The raw file descriptor.  -1 is used as an invalid value.
		 */
		int fd = -1;

		public:
		/**
		 * Explicitly construct a `FileDescriptor` object taking ownership of
		 * the file descriptor.
		 */
		explicit FileDescriptor(int newFD) : fd(newFD) {}

		/**
		 * Default constructor, constructs an invalid file descriptor.
		 */
		FileDescriptor() = default;

		/**
		 * Move construct.  Copy construction is not allowed.
		 */
		FileDescriptor(FileDescriptor &&other) : fd(other.fd)
		{
			other.fd = -1;
		}

		/**
		 * Move construct.  Takes ownership of the other file descriptor.
		 */
		FileDescriptor &operator=(FileDescriptor &&other)
		{
			if (other.fd != fd)
			{
				close();
				fd = std::exchange(other.fd, -1);
			}
			return *this;
		}

		FileDescriptor dup()
		{
			return FileDescriptor{::dup(fd)};
		}

		void close()
		{
			if (fd != -1)
			{
				::close(fd);
			}
			fd = -1;
		}

		/**
		 * Close the underlying file descriptor when the object is destroyed.
		 */
		~FileDescriptor()
		{
			close();
		}

		/**
		 * Take ownership of the file descriptor.
		 */
		int take()
		{
			return std::exchange(fd, -1);
		}

		/**
		 * Implicit conversion to int so that this can be used directly with
		 * system calls.  Note: Passing this to close will cause an unrelated
		 * file descriptor to be closed when the object is destroyed.
		 */
		operator int()
		{
			return fd;
		}

		/**
		 * Implicit conversion to bool, returns whether this is a valid file
		 * descriptor (assuming that nothing else has invalidated the file
		 * descriptor that this wraps).
		 */
		operator bool()
		{
			return fd != -1;
		}

		/**
		 * Apply Capsicum limits to this file descriptor.  The arguments are a
		 * list of `CAP_*` flags.
		 *
		 * This is security critical and so aborts on failure, there is no need
		 * to check the return value.
		 */
		template<typename... Args>
		void capsicum_limit(Args... args)
		{
			cap_rights_t rights;
			if (cap_rights_limit(fd, cap_rights_init(&rights, args...)) < 0)
			{
				perror("cap_rights_limit");
				exit(EXIT_FAILURE);
			}
		}
	};

	/**
	 * Connects a socket to the specified path.  The path should be a valid
	 * UNIX-domain sequential-packet socket.  Returns a valid file descriptor
	 * for the connected socket on success, an invalid file descriptor on
	 * failure.
	 */
	FileDescriptor connect_socket(const char *path)
	{
		FileDescriptor s{socket(PF_LOCAL, SOCK_SEQPACKET, 0)};
		sockaddr_un    address = {0};
		address.sun_len        = strlen(path);
		address.sun_family     = AF_UNIX;
		strlcpy(address.sun_path, path, sizeof(address.sun_path));
		struct stat sb;
		int         ret = connect(s,
                          reinterpret_cast<const struct sockaddr *>(&address),
                          sizeof(address));
		if (ret != 0)
		{
			s.close();
		}
		return s;
	}

	/**
	 * Send a message containing a file name and the associated file descriptor
	 * over `resultSocket`.  The socket must be a UNIX domain sequential
	 * message socket.
	 */
	int send_file(const std::string &fileName,
	              auto             &&fd,
	              FileDescriptor    &resultSocket)
	{
		struct msghdr header;
		struct iovec  iov
		{
			const_cast<void *>(static_cast<const void *>(fileName.data())),
			  fileName.size()
		};
		char ctrl_buf[CMSG_SPACE(sizeof(int))] = {0};
		header.msg_name                        = nullptr;
		header.msg_namelen                     = 0;
		header.msg_flags                       = 0;
		header.msg_iov                         = &iov;
		header.msg_iovlen                      = 1;
		header.msg_controllen                  = sizeof(ctrl_buf);
		header.msg_control                     = ctrl_buf;

		// Control message is the file descriptor.
		struct cmsghdr *cmsg                      = CMSG_FIRSTHDR(&header);
		cmsg->cmsg_level                          = SOL_SOCKET;
		cmsg->cmsg_type                           = SCM_RIGHTS;
		cmsg->cmsg_len                            = CMSG_LEN(sizeof(int));
		*reinterpret_cast<int *>(CMSG_DATA(cmsg)) = fd;
		int ret;
		do
		{
			ret = sendmsg(resultSocket, &header, MSG_NOSIGNAL | MSG_WAITALL);
		} while ((ret == -1) && (errno == EINTR));
		return ret;
	}

	/**
	 * Helper to receive a file over a socket.  Returns either the file name and
	 * a file descriptor for it on success or an error code on failure.
	 */
	std::variant<std::pair<std::string, FileDescriptor>, std::error_code>
	receive_file(FileDescriptor &s)
	{
		struct msghdr header;
		std::string   fileName;
		fileName.resize(MAXPATHLEN);
		int          fd = -1;
		struct iovec iov
		{
			static_cast<void *>(fileName.data()), fileName.size()
		};
		char ctrl_buf[CMSG_SPACE(sizeof(int))] = {0};

		header.msg_name       = nullptr;
		header.msg_namelen    = 0;
		header.msg_flags      = 0;
		header.msg_iov        = &iov;
		header.msg_iovlen     = 1;
		header.msg_controllen = sizeof(ctrl_buf);
		header.msg_control    = ctrl_buf;

		int ret;
		do
		{
			ret = recvmsg(s, &header, 0);
		} while ((ret == -1) && (errno == EINTR));
		if ((ret <= 0) || (header.msg_controllen == 0))
		{
			return std::error_code{errno, std::system_category()};
		}
		fileName.resize(ret);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
		if ((cmsg->cmsg_level == SOL_SOCKET) &&
		    (cmsg->cmsg_type == SCM_RIGHTS) &&
		    (cmsg->cmsg_len == CMSG_LEN(sizeof(int))))
		{
			fd = *reinterpret_cast<int *>(CMSG_DATA(cmsg));
		}
		return std::pair{std::move(fileName), FileDescriptor{fd}};
	}

} // namespace
