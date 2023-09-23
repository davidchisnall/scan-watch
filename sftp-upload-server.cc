#include "helpers.hh"
#include <bit>
#include <concepts>
#include <cxxabi.h>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <magic_enum_all.hpp>
#include <sstream>
#include <sys/capsicum.h>
#include <sys/mman.h>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace magic_enum::ostream_operators;

namespace
{
	/**
	 * For development, we enable a huge pile of debug output, written to a file
	 * in the user's home directory.  This is not enabled in release builds.
	 */
	constexpr bool EnableLogging =
#ifdef NDEBUG
	  false
#else
	  true
#endif
	  ;

	/// Helper concept to determine if something is an enumeration.
	template<typename T>
	concept Enum = std::is_enum_v<T>;

	/**
	 * Concept for enumerations or integers.  These can be automatically read
	 * and written over the connection.
	 */
	template<typename T>
	concept Scalar = std::integral<T> || Enum<T>;

	/**
	 * Byte swap any integral type if the host byte order is not network byte
	 * order.  Note: there is no modern C++ compiler for VAX, so we assume
	 * endian is either big or little.
	 */
	template<std::integral T>
	T byte_swap(T &value)
	{
		if constexpr ((sizeof(T) > 1) &&
		              (std::endian::native != std::endian::big))
		{
			value = std::byteswap(value);
		}
		return value;
	}

	/**
	 * Byte-swap an enum value, of host and networ byte order differ.
	 */
	template<Enum T>
	T byte_swap(T &value)
	{
		std::underlying_type_t<T> v = static_cast<decltype(v)>(value);
		byte_swap(v);
		value = T(v);
		return value;
	}

	/**
	 * Read a value of type T from standard input, byte swapping it if required.
	 */
	template<Scalar T>
	T read_scalar()
	{
		T ret;
		std::cin.read(reinterpret_cast<char *>(&ret), sizeof(T));
		return byte_swap(ret);
	}

	/**
	 * Request types, from the RFC.  Most are not supported.
	 */
	enum class PacketType : uint8_t
	{
		SSH_FXP_INIT           = 1,
		SSH_FXP_VERSION        = 2,
		SSH_FXP_OPEN           = 3,
		SSH_FXP_CLOSE          = 4,
		SSH_FXP_READ           = 5,
		SSH_FXP_WRITE          = 6,
		SSH_FXP_LSTAT          = 7,
		SSH_FXP_FSTAT          = 8,
		SSH_FXP_SETSTAT        = 9,
		SSH_FXP_FSETSTAT       = 10,
		SSH_FXP_OPENDIR        = 11,
		SSH_FXP_READDIR        = 12,
		SSH_FXP_REMOVE         = 13,
		SSH_FXP_MKDIR          = 14,
		SSH_FXP_RMDIR          = 15,
		SSH_FXP_REALPATH       = 16,
		SSH_FXP_STAT           = 17,
		SSH_FXP_RENAME         = 18,
		SSH_FXP_READLINK       = 19,
		SSH_FXP_LINK           = 21,
		SSH_FXP_BLOCK          = 22,
		SSH_FXP_UNBLOCK        = 23,
		SSH_FXP_STATUS         = 101,
		SSH_FXP_HANDLE         = 102,
		SSH_FXP_DATA           = 103,
		SSH_FXP_NAME           = 104,
		SSH_FXP_ATTRS          = 105,
		SSH_FXP_EXTENDED       = 200,
		SSH_FXP_EXTENDED_REPLY = 201
	};

	/**
	 * Error code values from the RFC.  Most are unused.
	 */
	enum class ErrorCode : uint32_t
	{
		SSH_FX_OK                          = 0,
		SSH_FX_EOF                         = 1,
		SSH_FX_NO_SUCH_FILE                = 2,
		SSH_FX_PERMISSION_DENIED           = 3,
		SSH_FX_FAILURE                     = 4,
		SSH_FX_BAD_MESSAGE                 = 5,
		SSH_FX_NO_CONNECTION               = 6,
		SSH_FX_CONNECTION_LOST             = 7,
		SSH_FX_OP_UNSUPPORTED              = 8,
		SSH_FX_INVALID_HANDLE              = 9,
		SSH_FX_NO_SUCH_PATH                = 10,
		SSH_FX_FILE_ALREADY_EXISTS         = 11,
		SSH_FX_WRITE_PROTECT               = 12,
		SSH_FX_NO_MEDIA                    = 13,
		SSH_FX_NO_SPACE_ON_FILESYSTEM      = 14,
		SSH_FX_QUOTA_EXCEEDED              = 15,
		SSH_FX_UNKNOWN_PRINCIPAL           = 16,
		SSH_FX_LOCK_CONFLICT               = 17,
		SSH_FX_DIR_NOT_EMPTY               = 18,
		SSH_FX_NOT_A_DIRECTORY             = 19,
		SSH_FX_INVALID_FILENAME            = 20,
		SSH_FX_LINK_LOOP                   = 21,
		SSH_FX_CANNOT_DELETE               = 22,
		SSH_FX_INVALID_PARAMETER           = 23,
		SSH_FX_FILE_IS_A_DIRECTORY         = 24,
		SSH_FX_BYTE_RANGE_LOCK_CONFLICT    = 25,
		SSH_FX_BYTE_RANGE_LOCK_REFUSED     = 26,
		SSH_FX_DELETE_PENDING              = 27,
		SSH_FX_FILE_CORRUPT                = 28,
		SSH_FX_OWNER_INVALID               = 29,
		SSH_FX_GROUP_INVALID               = 30,
		SSH_FX_NO_MATCHING_BYTE_RANGE_LOCK = 31,
	};

	/**
	 * File attributes from the RFC.  Most are unused.
	 */
	enum class FileAttributes : uint32_t
	{
		SSH_FILEXFER_ATTR_SIZE              = 0x00000001,
		SSH_FILEXFER_ATTR_PERMISSIONS       = 0x00000004,
		SSH_FILEXFER_ATTR_ACCESSTIME        = 0x00000008,
		SSH_FILEXFER_ATTR_CREATETIME        = 0x00000010,
		SSH_FILEXFER_ATTR_MODIFYTIME        = 0x00000020,
		SSH_FILEXFER_ATTR_ACL               = 0x00000040,
		SSH_FILEXFER_ATTR_OWNERGROUP        = 0x00000080,
		SSH_FILEXFER_ATTR_SUBSECOND_TIMES   = 0x00000100,
		SSH_FILEXFER_ATTR_BITS              = 0x00000200,
		SSH_FILEXFER_ATTR_ALLOCATION_SIZE   = 0x00000400,
		SSH_FILEXFER_ATTR_TEXT_HINT         = 0x00000800,
		SSH_FILEXFER_ATTR_MIME_TYPE         = 0x00001000,
		SSH_FILEXFER_ATTR_LINK_COUNT        = 0x00002000,
		SSH_FILEXFER_ATTR_UNTRANSLATED_NAME = 0x00004000,
		SSH_FILEXFER_ATTR_CTIME             = 0x00008000,
		SSH_FILEXFER_ATTR_EXTENDED          = 0x80000000
	};

	/**
	 * File types, from the File Attributes section of the RFC.
	 */
	enum class FileType : uint8_t
	{
		SSH_FILEXFER_TYPE_REGULAR      = 1,
		SSH_FILEXFER_TYPE_DIRECTORY    = 2,
		SSH_FILEXFER_TYPE_SYMLINK      = 3,
		SSH_FILEXFER_TYPE_SPECIAL      = 4,
		SSH_FILEXFER_TYPE_UNKNOWN      = 5,
		SSH_FILEXFER_TYPE_SOCKET       = 6,
		SSH_FILEXFER_TYPE_CHAR_DEVICE  = 7,
		SSH_FILEXFER_TYPE_BLOCK_DEVICE = 8,
		SSH_FILEXFER_TYPE_FIFO         = 9,

	};

	/**
	 * Log file if logging is enabled.  If logging is not enabled, this is a
	 * trivial value that the compiler can elide.
	 */
	std::conditional_t<EnableLogging, std::ofstream, char> logfile;

	/**
	 * Log a formatted message, if and only if logging is enabled.
	 */
	template<typename... Args>
	void log(std::string_view msg, Args... args)
	{
		if constexpr (EnableLogging)
		{
			logfile << std::vformat(msg, std::make_format_args(args...))
			        << std::endl;
		}
	}

	/**
	 * If `condition` is false, abort.  If logging is enabled, log the
	 * formatted error message.
	 */
	template<typename... Args>
	void expect(bool condition, std::string_view msg, Args... args)
	{
		if (!condition)
		{
			log(msg, std::forward<Args>(args)...);
			exit(EXIT_FAILURE);
		}
	}

	class ReplyBuilder;

	/**
	 * Type representing a request.  This provides accessors for reading values
	 * from standard in, up to the end of the request.
	 *
	 * Constructing an instance of this will read the size, type, and request
	 * ID from the stream.
	 *
	 * Note: Packets read from standard in during their life.  Only a single
	 * `Request` should exist at any given time.
	 */
	struct Request
	{
		/**
		 * Size of the request that hasn't been consumed from the input yet.
		 * Excludes the size of the type at the start.
		 */
		uint32_t size =
		  read_scalar<uint32_t>() - sizeof(type) - sizeof(requestID);

		/**
		 * Request type.
		 */
		PacketType type = read_scalar<PacketType>();

		/**
		 * The request ID for this request.
		 */
		uint32_t requestID = read_scalar<uint32_t>();

		/**
		 * Constant to define the maximum size for a request.  This is somewhat
		 * arbitrary and is mostly to avoid denial of service problems.
		 */
		const uint32_t MaxSize = 10240;

		/**
		 * Try to read a value of type T from the request, returns a valid
		 * optional if successful or an invalid one if not.
		 */
		template<typename T>
		std::optional<T> try_consume();

		/**
		 * Reads a request from the input.  If the size is invalid, aborts.
		 */
		Request()
		{
			expect(size <= MaxSize, "Invalid size: {}", size);
		}

		/**
		 * Specialisation of try_consume for scalar types.  Reads them from the
		 * stream, byte swapping if required.
		 */
		template<typename T>
		std::optional<T> try_consume()
		    requires Scalar<T>
		{
			if (size < sizeof(T))
			{
				return std::nullopt;
			}
			size -= sizeof(T);
			return read_scalar<T>();
		};

		/**
		 * Try to read a string from the input.  Strings are stored as a 32-bit
		 * length followed by the string data.
		 */
		template<>
		std::optional<std::string> try_consume()
		{
			auto length = try_consume<uint32_t>();
			if (!length)
			{
				return std::nullopt;
			}
			expect(
			  *length <= size,
			  "Reading {}-byte string from request with only {} bytes left",
			  *length,
			  size);
			log("Reading {}-byte string from request ({} bytes remaining in "
			    "request)",
			    *length,
			    size);
			std::stringstream out;
			for (uint32_t i = 0; i < length; ++i)
			{
				char c = std::cin.get();
				out << c;
			}
			auto result = out.str();
			log("Read string {}", result);
			size -= *length;
			return result;
		}

		/**
		 * Consume a value from the request, killing the process if the value is
		 * not present.
		 */
		template<typename T>
		T consume()
		{
			auto opt = try_consume<T>();
			if (!opt)
			{
				if constexpr (EnableLogging)
				{
					int   status;
					char *typeName = abi::__cxa_demangle(
					  typeid(T).name(), NULL, NULL, &status);
					log("Unable to read value of type '{}' from request ({} "
					    "bytes left)",
					    typeName,
					    size);
				}
				exit(EXIT_FAILURE);
			}
			return *opt;
		}

		/**
		 * Ignore the rest of the request when the object is destroyed.
		 */
		~Request()
		{
			if (size > 0)
			{
				log("Ignoring {} bytes in request", size);
				std::cin.ignore(size);
			}
		}

		/**
		 * Build a reply to this request, with the specified reply type.
		 */
		ReplyBuilder reply(PacketType t);
	};

	/**
	 * Class that builds a reply message.  This buffers the reply data before
	 * sending so that users do not need to compute the size of a response
	 * before sending it.
	 *
	 * The response is automatically sent when this object is destroyed, if it
	 * has not been.
	 */
	class ReplyBuilder
	{
		/// The buffer used to hold the reply.
		std::vector<char> buffer;

		public:
		/**
		 * Construct a reply of the specific type.  If this includes a request
		 * ID, the caller must provide it with `append`.
		 */
		ReplyBuilder(PacketType t)
		{
			append(t);
		}

		/**
		 * Construct a response of the specified type, taking the request ID
		 * from the request.
		 */
		ReplyBuilder(const Request &h, PacketType t)
		{
			append(t);
			append(h.requestID);
		}

		/**
		 * Send the response.
		 */
		void send()
		{
			uint32_t length = buffer.size();
			log("Response length: {}", length);
			byte_swap(length);
			// Log the entire contents of the reply, one byte at a time.
			if constexpr (EnableLogging)
			{
				for (char c : buffer)
				{
					log("Byte: {} ({:#x})", c, c);
				}
			}
			std::cout.write(reinterpret_cast<char *>(&length), sizeof(length));
			std::cout.write(buffer.data(), buffer.size());
			buffer.clear();
		}

		/**
		 * Destructor.  Sends the response if it has not already been sent.
		 */
		~ReplyBuilder()
		{
			if (!buffer.empty())
			{
				send();
			}
		}

		/**
		 * Append a scalar value to the response, byte swapping if required.
		 */
		template<Scalar T>
		ReplyBuilder &append(T value)
		{
			byte_swap(value);
			buffer.insert(buffer.end(),
			              reinterpret_cast<char *>(&value),
			              reinterpret_cast<char *>(&value) + sizeof(T));
			return *this;
		}

		/**
		 * Append a string to the response.
		 */
		ReplyBuilder &append(std::string_view string)
		{
			append<uint32_t>(string.size());
			buffer.insert(buffer.end(), string.begin(), string.end());
			return *this;
		}
	};

	ReplyBuilder Request::reply(PacketType t)
	{
		return {*this, t};
	}

	/**
	 * The UNIX domain socket that we we will write the output to.
	 */
	FileDescriptor resultSocket;

	/**
	 * Map from exported file ID to the name (full path) and file descriptor
	 * for the file.
	 */
	std::unordered_map<uint64_t, std::pair<std::string, FileDescriptor>>
	  openFiles;

	/**
	 * Map from exported file ID to the name (full path) of open directories.
	 *
	 * All directories are faked.
	 */
	std::unordered_map<uint64_t, std::string> openDirectories;

	/**
	 * The next ID to use for external handles.
	 */
	uint64_t nextFileID;

	/**
	 * Build a status message as a response.  Optionally, an error code and
	 * message can be provided for error returns (though, in most cases, we
	 * simply abort in error conditions).
	 */
	ReplyBuilder reply_with_status(Request         &h,
	                               ErrorCode        ec = ErrorCode::SSH_FX_OK,
	                               std::string_view message = "")
	{
		log("Preparing response {}", message);
		auto reply = h.reply(PacketType::SSH_FXP_STATUS);
		reply.append(ec).append(message).append("en-us");
		return reply;
	}

	/**
	 * Templated function for handling incoming requests of a specific kind. The
	 * generic case is hit for any unhandled types and simply exits.
	 */
	template<PacketType Type>
	void handle(Request &h)
	{
		expect(false, "Unhandled request type {}", magic_enum::enum_name(Type));
	}

	/**
	 * Handle open requests.  We support only creation of new files for writing,
	 * so this is very simple.  This constructs a new anonymous shared memory
	 * object, which will then be passed to the consumer process.
	 */
	template<>
	void handle<PacketType::SSH_FXP_OPEN>(Request &h)
	{
		// Ignore flags and attributes.
		auto           name = h.consume<std::string>();
		FileDescriptor fd{memfd_create(name.c_str(), 0)};
		int            id = nextFileID++;
		openFiles.emplace(id, std::pair{name, std::move(fd)});
		std::stringstream out;
		out << id;
		h.reply(PacketType::SSH_FXP_HANDLE).append(out.str());
	}

	/**
	 * Handle close requests.  This passes the new object to the consumer.
	 */
	template<>
	void handle<PacketType::SSH_FXP_CLOSE>(Request &h)
	{
		auto handleString = h.consume<std::string>();
		int  handle       = std::stod(handleString);
		auto file         = openFiles.find(handle);
		if (file == openFiles.end())
		{
			auto dir = openDirectories.find(handle);
			expect(dir != openDirectories.end(),
			       "File {} is not open",
			       handleString);
			openDirectories.erase(dir);
			reply_with_status(h);
			return;
		}
		auto        fileName = file->second.first;
		auto       &fd       = file->second.second;
		struct stat sb       = {0};
		fstat(fd, &sb);
		log("Closing file '{}' with {} bytes received",
		    file->second.first,
		    sb.st_size);
		reply_with_status(h);
		int ret = send_file(fileName, fd, resultSocket);
		expect(ret >= 0, "sendmsg failed: {}", strerror(errno));
		openFiles.erase(file);
	}

	/**
	 * Handle directory read requests.  All directories are empty.
	 */
	template<>
	void handle<PacketType::SSH_FXP_READDIR>(Request &h)
	{
		// Ignore the path request and pretend everything is fine.
		reply_with_status(h, ErrorCode::SSH_FX_EOF, "No more files");
	}

	/**
	 * Handle writes into uploaded files.
	 */
	template<>
	void handle<PacketType::SSH_FXP_WRITE>(Request &h)
	{
		auto handleString = h.consume<std::string>();
		auto offset       = h.consume<uint64_t>();
		log("Offset is {}", offset);
		auto data   = h.consume<std::string>();
		int  handle = std::stod(handleString);
		auto file   = openFiles.find(handle);
		expect(file != openFiles.end(), "File {} is not open", handleString);
		auto &fd = file->second.second;
		log("Found file {} ({})", handleString, static_cast<int>(fd));
		log("Writing {} bytes into {} at offset {}",
		    data.size(),
		    file->second.first,
		    offset);
		lseek(fd, offset, SEEK_SET);
		int written = 0;
		int ret;
		while (
		  (ret = write(fd, data.c_str() + written, data.size() - written)) != 0)
		{
			if ((ret < 0) && (errno == EINTR))
			{
				continue;
			}
			expect(ret > 0, "write returned {}", strerror(errno));
			written += ret;
		}
		reply_with_status(h);
	}

	/**
	 * Handle directory open requests.
	 */
	template<>
	void handle<PacketType::SSH_FXP_OPENDIR>(Request &h)
	{
		auto dir = h.consume<std::string>();
		log("Opening fake directory {}", dir);
		int id = nextFileID++;
		openDirectories.emplace(id, std::move(dir));
		std::stringstream out;
		out << id;
		h.reply(PacketType::SSH_FXP_HANDLE).append(out.str());
	}

	/**
	 * Canonicalise paths.  We export a tiny virtual filesystem and
	 * canonicalise . to / and do no other canonicalisation.
	 */
	template<>
	void handle<PacketType::SSH_FXP_REALPATH>(Request &h)
	{
		auto path     = h.consume<std::string>();
		auto resolved = path;
		if (path == ".")
		{
			resolved = "/";
		}
		h.reply(PacketType::SSH_FXP_NAME)
		  .append(1) // count
		  .append(resolved)
		  .append(path)
		  .append<uint32_t>(0);
	}

	/**
	 * Handle a small subset of stat messages, used by openssh to determine
	 * whether it can upload into a directory.
	 */
	template<>
	void handle<PacketType::SSH_FXP_STAT>(Request &h)
	{
		auto path = h.consume<std::string>();
		log("Stat '{}'", path);
		// We're going to assume everything that's stat'd is a directory.
		// Note: The RFC requires a mandatory 1-byte field after the flags.
		// OpenSSH does not expect this and, if it is present, will read
		// everything else with an off-by-one index, so we omit it.
		// Similarly, OpenSSH ignores the flag telling you that this is a
		// directory and instead requires the permissions field to contain stat
		// status values.
		h.reply(PacketType::SSH_FXP_ATTRS)
		  .append(
		    FileAttributes::SSH_FILEXFER_ATTR_PERMISSIONS) // Permissions, no
		                                                   // other attributes.
		  .append<uint32_t>(S_IFDIR); // Directory permission
	}

	/**
	 * SFINAE helper to initialise the logging file.
	 */
	template<bool LoggingEnabled = EnableLogging>
	std::enable_if_t<LoggingEnabled, void> *init_log_file()
	{
		logfile = std::conditional_t<LoggingEnabled, std::ofstream, void>{
		  "sftp.log", std::ios::out | std::ios::ate | std::ios::app};
		return nullptr;
	}

	/**
	 * SFINAE helper to not initialise the logging file.
	 */
	template<bool LoggingEnabled = EnableLogging>
	std::enable_if_t<!LoggingEnabled, void> init_log_file()
	{
	}
} // namespace

int main()
{
	// Don't open the log file if this isn't a debug build.
	init_log_file();
	const char *socketPath = "sftp.sock";
	resultSocket           = connect_socket(socketPath);
	expect(
	  resultSocket, "Failed to connect to result socket: {}", strerror(errno));

	cap_enter();

	{
		// Request ID here is actually the protocol version.
		Request initPacket;
		expect(initPacket.type == PacketType::SSH_FXP_INIT,
		       "Unexpected first packet");
		// Ignore the version that the client sends and use version 3.  This is
		// what OpenSSH expects.
		ReplyBuilder(PacketType::SSH_FXP_VERSION).append<uint32_t>(3);
	}
	log("Sent version response");

	while (!std::cin.eof())
	{
		Request header;
		log("{} bytes of payload", header.size);
		log("Request type: {} ({})",
		    magic_enum::enum_name(header.type),
		    static_cast<unsigned>(header.type));
		log("Request ID: {}", header.requestID);
		// Dispatch to the correct handler.
		magic_enum::enum_switch(
		  [&](auto type) {
			  constexpr PacketType Type = type;
			  handle<Type>(header);
		  },
		  header.type);
	}
}
