#define SOL_ALL_SAFETIES_ON 1
#include <CLI/CLI.hpp>
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/renderer.h>

#include <rapidfuzz/fuzz.hpp>

#include <sol/sol.hpp>

#include <cerrno>
#include <compare>
#include <concepts>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdio.h>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/procdesc.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "helpers.hh"

/// Short name for the Tesseract base API
using BaseAPI = tesseract::TessBaseAPI;

/// The path to tesseract's data files.
std::string tesseractDatapath = "/usr/local/share/tessdata";

/**
 * Final output location.
 */
std::string outputPath = "/tmp/ocr";

/**
 * Vector of directory names and their descriptors for files that were opened
 */
std::vector<std::pair<std::string, FileDescriptor>> preopenedFiles;

/// Socket for sending files to be saved.
FileDescriptor saveOutput;

/**
 * File handle for the current output file.  This is set in `open` if and only
 * if the file being created matches the value in `currentOutputFileName`.
 */
FileDescriptor currentOutputFile;

/**
 * Name of the current output file.  If a file of this name is opened, a copy
 * of the file descriptor is stored in currentOutputFile.
 */
std::string currentOutputFileName;

/**
 * Map of names to file descriptors for all of the shared memory objects that
 * we've opened.
 */
std::unordered_map<std::string, FileDescriptor> fakeFiles;

constexpr enum LogLevel {
	Status,
	Verbose
} Log =
#ifdef NDEBUG
  Status
#else
  Verbose
#endif
  ;

/**
 * Log a formatted message, if and only if logging is enabled.
 */
template<LogLevel Level = Status, typename... Args>
void log(std::string_view msg, Args... args)
{
	if constexpr (Log >= Level)
	{
		std::cerr << std::vformat(msg, std::make_format_args(args...))
		          << std::endl;
	}
}

/**
 * If `condition` is false, abort.  If logging is enabled, log the
 * formatted error message.
 */
template<LogLevel Level = Status, typename... Args>
void expect(bool condition, std::string_view msg, Args... args)
{
	if (!condition)
	{
		log<Level>(msg, std::forward<Args>(args)...);
		exit(EXIT_FAILURE);
	}
}

/**
 * Class representing a page.  These are collected before being emitted.
 */
class Page
{
	/**
	 * The page number.
	 */
	int pageNumber;

	/**
	 * The Tesseract API that has recognised the text for this page.
	 */
	std::unique_ptr<BaseAPI> api;

	public:
	/**
	 * Constructor, takes ownership of the Tesseract API.
	 */
	Page(std::unique_ptr<BaseAPI> &&api, int page)
	  : api(std::move(api)), pageNumber(page)
	{
	}

	Page(Page &&other) = default;

	/**
	 * Destructor, cleans up the stashed copy of the input 'file'.
	 */
	~Page()
	{
		if (api)
		{
			fakeFiles.erase(api->GetInputName());
		}
	}

	/**
	 * Comparison operator.  Sorts by page number.
	 */
	auto operator<=>(const Page &other) const
	{
		return pageNumber <=> other.pageNumber;
	}

	/**
	 * Returns the page number.
	 */
	unsigned page_number()
	{
		return pageNumber;
	}

	/**
	 * Returns a reference to the tesseract API that this owns.
	 */
	BaseAPI &base_api() const
	{
		return *api;
	}
};

/**
 * Document class.  Only one of these is live at a time.  Collects pages and
 * emits a PDF once it has a full set.
 */
class Document
{
	/**
	 * The pages in this document, indexed by page number.
	 */
	std::map<int, Page> pages;

	/**
	 * The page count.  -1 indicates an unknown number of pages.
	 */
	size_t pageCount = -1;

	/**
	 * The title for this document.
	 */
	std::string title;

	public:
	/**
	 * Add a page.  Inserts it at the expected page number, or at the end if
	 * there is already a page with the same number.
	 *
	 * If this is the last page, generates the PDF.
	 */
	void add_page(Page &&p)
	{
		int pageNumber = p.page_number();
		// If this is a duplicate page, put it in a new place.
		while (pages.find(pageNumber) != pages.end())
		{
			pageNumber++;
		}
		pages.emplace(pageNumber, std::move(p));
		if (pages.size() == pageCount)
		{
			finish();
		}
	}

	/**
	 * Sets the page count.
	 */
	void set_page_count(size_t count)
	{
		pageCount = count;
	}

	/**
	 * Sets the title.  If this document has a title already, combines it with
	 * the existing one.
	 */
	void set_title(std::string &&newTitle)
	{
		if (title != newTitle)
		{
			title += newTitle;
		}
	}

	/**
	 * Returns true if all pages in this document have been handled.
	 */
	bool is_finished()
	{
		return pages.empty();
	}

	/**
	 * Finishes processing this document, emits the PDF.
	 */
	void finish()
	{
		if (is_finished())
		{
			return;
		}
		std::string output = "/tmp/ocr/";
		output += title.empty() ? "untitled" : title;
		currentOutputFileName = output + ".pdf";
		auto renderer         = std::make_unique<tesseract::TessPDFRenderer>(
          output.c_str(), tesseractDatapath.c_str(), /*textonly*/ false);
		renderer->BeginDocument(title.c_str());
		for (auto &p : pages)
		{
			renderer->AddImage(&p.second.base_api());
		}
		renderer->EndDocument();
		expect(
		  send_file(currentOutputFileName, currentOutputFile, saveOutput) >= 0,
		  "failed to send file {}: {}",
		  currentOutputFileName,
		  strerror(errno));

		// Reset this object to its default state.
		pageCount = -1;
		title.clear();
		pages.clear();
		// Clean up any left over temporary files.
		currentOutputFile.close();
		fakeFiles.clear();
	}
};

/**
 * A chunk of text recognised by OCR.  Passed to Lua.
 */
class TextChunk
{
	public:
	/// The text for this fragment.
	std::string text;
	/// The confidence that the OCR engine places in this recognition.
	float confidence;
	/// Is this bold text?
	bool isBold;
	/// Is this italic text?
	bool isItalic;
	/// Is this underlined text?
	bool isUnderlined;
	/// Is this monospaced text?
	bool isMonospace;
	/// Is this serif text?
	bool isSerif;
	/// Is this small-caps text?
	bool isSmallCap;
	/// The point size of the recognised text.
	int pointSize;

	public:
	/**
	 * Construct a text chunk from a tesseract text iterator.
	 */
	TextChunk(tesseract::ResultIterator *ri)
	{
		int fontID;
		ri->WordFontAttributes(&isBold,
		                       &isItalic,
		                       &isUnderlined,
		                       &isMonospace,
		                       &isSerif,
		                       &isSmallCap,
		                       &pointSize,
		                       &fontID);
		std::unique_ptr<const char[]> word{
		  ri->GetUTF8Text(tesseract::RIL_TEXTLINE)};
		text = std::string{word.get()};
	}

	/**
	 * Register this type with a lua context.
	 */
	static void register_usertype(sol::state &lua)
	{
		sol::usertype<TextChunk> chunkType =
		  lua.new_usertype<TextChunk>("TextChunk", sol::no_constructor);
		chunkType.set("text", sol::readonly(&TextChunk::text));
		chunkType.set("confidence", sol::readonly(&TextChunk::confidence));
		chunkType.set("isBold", sol::readonly(&TextChunk::isBold));
		chunkType.set("isItalic", sol::readonly(&TextChunk::isItalic));
		chunkType.set("isUnderlined", sol::readonly(&TextChunk::isUnderlined));
		chunkType.set("isMonospace", sol::readonly(&TextChunk::isMonospace));
		chunkType.set("isSerif", sol::readonly(&TextChunk::isSerif));
		chunkType.set("isSmallCap", sol::readonly(&TextChunk::isSmallCap));
		chunkType.set("pointSize", sol::readonly(&TextChunk::pointSize));
	}

	/**
	 * Extract text from a tesseract API.  Returns a vector of
	 */
	static auto extract_text(BaseAPI &api)
	{
		tesseract::ResultIterator   *ri    = api.GetIterator();
		tesseract::PageIteratorLevel level = tesseract::RIL_TEXTLINE;
		auto chunks = std::make_shared<std::vector<TextChunk>>();
		if (ri != 0)
		{
			do
			{
				chunks->emplace_back(ri);
			} while (ri->Next(level));
		}
		return chunks;
	}
};

/**
 * A Lua matcher that recognises uploaded files.
 */
class LuaMatcher
{
	/// Lua VM for this matcher
	sol::state lua;

	public:
	/**
	 * The test function for detecting whether a given page matches this input.
	 */
	std::function<double(sol::nested<std::vector<TextChunk>>)> test;

	/**
	 * The match function for deciding how to handle a recognised page.
	 */
	std::function<sol::table(sol::nested<std::vector<TextChunk>>)> match;

	/**
	 * Construct a matcher from Lua code in the specified file.
	 */
	LuaMatcher(const std::string &script)
	{
		lua.open_libraries(
		  sol::lib::base, sol::lib::package, sol::lib::string, sol::lib::math);

		// Expose fuzzy matching string functions
		lua.set_function(
		  "ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::ratio(str1, str2);
		  });
		lua.set_function(
		  "partial_ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::partial_ratio(str1, str2);
		  });
		lua.set_function(
		  "token_sort_ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::token_sort_ratio(str1, str2);
		  });
		lua.set_function(
		  "token_set_ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::token_set_ratio(str1, str2);
		  });
		lua.set_function(
		  "partial_token_set_ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::partial_token_set_ratio(str1, str2);
		  });
		lua.set_function(
		  "partial_token_sort_ratio",
		  [](const std::string_view str1, const std::string_view str2) {
			  return rapidfuzz::fuzz::partial_token_sort_ratio(str1, str2);
		  });

		// Register the text chunk type.
		TextChunk::register_usertype(lua);

		// Load the script
		lua.script_file(script);

		test  = lua["test"];
		match = lua["match"];
		expect(static_cast<bool>(test),
		       "Lua file {} did not contain a test function",
		       script);
		expect(static_cast<bool>(match),
		       "Lua file {} did not contain a match function",
		       script);
	}

	/**
	 * Run the garbage collector now.
	 */
	void gc()
	{
		lua.collect_garbage();
	}
};

/**
 * Collection of all of the matchers.
 */
std::vector<LuaMatcher> matchers;

/**
 * The current document being assembled.
 */
Document currentDocument;

/**
 * Process a new page.  The name and file descriptor are both provided over a
 * pipe, the file may not exist in the filesystem at all.
 */
void process_page(std::string &&filename, FileDescriptor &&file)
{
	// The last matcher that we found.
	static LuaMatcher *lastMatcher;

	// Stash the received file as a temporary.  It may be used when writing the
	// PDF.
	fakeFiles[filename] = file.dup();
	// File stream for reading the received file.
	std::unique_ptr<FILE, decltype(&fclose)> fileStream{fdopen(file, "r"),
	                                                    &fclose};

	// Read the image.
	Pix *image = pixReadStream(fileStream.get(), 0);
	// If this is a valid image, pass it forward
	if (!image)
	{
		log<Verbose>("Failed to parse image: {}", filename);
		return;
	}

	if (send_file(filename, fileno(fileStream.get()), saveOutput) < 0)
	{
		perror("sendmsg");
		exit(EXIT_FAILURE);
	}

	auto api = std::make_unique<BaseAPI>();
	expect<Status>(api->Init(tesseractDatapath.c_str(), "eng") == 0,
	               "Failed to initialize tesseract");

	api->SetImage(image);
	api->SetInputName(filename.c_str());
	api->Recognize(nullptr);
	auto        text              = TextChunk::extract_text(*api);
	LuaMatcher *matcher           = nullptr;
	double      highestConfidence = 0;
	for (auto &m : matchers)
	{
		double confidence = m.test(*text);
		m.gc();
		if (confidence > highestConfidence)
		{
			highestConfidence = confidence;
			matcher           = &m;
		}
		// If we've found a high-confidence match, stop.
		if (confidence >= 100)
		{
			break;
		}
	}

	if (highestConfidence > 0)
	{
		log<Verbose>("Match confidence {}", highestConfidence);
		auto matchResult = matcher->match(*text);
		matcher->gc();
		auto [title, page, count, discard, finish] =
		  matchResult.get<std::optional<std::string>,
		                  std::optional<int>,
		                  std::optional<int>,
		                  std::optional<bool>,
		                  std::optional<bool>>(
		    "title", "page", "count", "discard", "finish");
		if (discard && *discard)
		{
			log<Verbose>("Discarding page");
			fakeFiles.erase(filename);
			return;
		}
		if (matcher != lastMatcher)
		{
			currentDocument.finish();
			lastMatcher = matcher;
		}
		if (title)
		{
			currentDocument.set_title(std::move(*title));
		}
		if (count)
		{
			currentDocument.set_page_count(*count);
		}
		currentDocument.add_page({std::move(api), page.value_or(-1)});
		if (finish)
		{
			currentDocument.finish();
		}
		if (currentDocument.is_finished())
		{
			lastMatcher = nullptr;
		}
	}
}

/**
 * Wait for files over a socket and write them to disk.
 *
 * This runs in a child process that has write access to the output directory.
 * This process is responsible for ensuring that files are not ever overwritten.
 */
[[noreturn]] void wait_for_files(int rawSocket)
{
	FileDescriptor s{rawSocket};
	// Open the output directory and set
	FileDescriptor outputDirectory{open(outputPath.c_str(), O_DIRECTORY)};
	expect(outputDirectory, "Child process failed to open {}: {}", outputPath, strerror(errno));
	outputDirectory.capsicum_limit(CAP_PWRITE, CAP_CREATE, CAP_LOOKUP);
	expect(outputDirectory, "Child process failed to limit rights on {}: {}", outputPath, strerror(errno));
	// Drop privileges.
	cap_enter();
	log<Verbose>("Child process starting to write to {}", outputPath);
	// Helper to open the file in the output directory.  Creates a new file.  If
	// the file exists, appends a suffix until it is unique.
	auto openFile = [&](std::filesystem::path path) {
		auto stem      = path.stem().string();
		int  suffix    = 0;
		auto extension = path.extension().string();

		do
		{
			FileDescriptor outFD{openat(outputDirectory,
			                            path.c_str(),
			                            O_WRONLY | O_CREAT | O_EXCL,
			                            0600)};
			if (outFD)
			{
				log<Verbose>("Opened output file {}", path.string());
				return outFD;
			}
			std::stringstream s;
			s << stem << '-' << suffix++ << extension;
			path.replace_filename(s.str());
			log<Verbose>("Trying new filename {}", path.string());
			// Give up after a bounded number of tries.  If we hit this case, we're probably under attack.
		} while (suffix < 2);
		return FileDescriptor{};
	};

	while (true)
	{
		auto result = receive_file(s);
		if (auto *msg =
		      std::get_if<std::pair<std::string, FileDescriptor>>(&result))
		{
			auto &[fileName, fd] = *msg;
			log<Verbose>(
			  "Child received file '{}' ({})", fileName, static_cast<int>(fd));
			std::filesystem::path fn{fileName};

			// Open a new file at `path` and write the received input into it.
			auto copy = [&, &fd = fd](auto &&path) {
				FileDescriptor outFD     = openFile(path);
				int            ret       = 0;
				off_t          inOffset  = 0;
				off_t          outOffset = 0;
				char           b;
				do
				{
					ret = copy_file_range(
					  fd, &inOffset, outFD, &outOffset, SSIZE_MAX, 0);
					if (ret < 0)
					{
						log<Verbose>("copy_file_range failed with {}",
						             strerror(errno));
					}
				} while (ret > 0);
				// copy_file_range does not currently work from shared memory
				// objects.
				if ((ret < 0) && (errno == EINVAL))
				{
					log<Verbose>("Fallback copy path");
					const size_t size = 1024;
					char         buffer[size];
					lseek(fd, 0, SEEK_SET);
					lseek(outFD, 0, SEEK_SET);
					// read/write helper that retries on interrupt.
					auto rw = [&](auto fn, int fd, size_t size) {
						int ret;
						do
						{
							ret = fn(fd, buffer, size);
						} while ((ret == -1) && (errno == EAGAIN));
						return ret;
					};
					int bytesRead;
					while ((bytesRead = rw(read, fd, size)) > 0)
					{
						while ((bytesRead > 0) &&
						       (ret = rw(write, outFD, bytesRead)) > 0)
						{
							bytesRead -= ret;
						}
					}
				}
			};
			// Store jpegs in the input directory.
			if (fn.extension() == ".jpg")
			{
				copy(std::filesystem::path{"raw_inputs"} / fn.filename());
			}
			// Store PDFs in the output directory.
			else if (fn.extension() == ".pdf")
			{
				copy(std::filesystem::path{"output"} / fn.filename());
			}
		}
		else
		{
			log<Verbose>("Child process error from socket: ",
			             strerror(std::get<std::error_code>(result).value()));
			break;
		}
	}
	exit(EXIT_SUCCESS);
}

/**
 * Start the child executing and set up the socket to send completed files for
 * writing to the filesystem.
 */
void start_child()
{
	int outputSockets[2] = {-1, -1};
	if (socketpair(PF_LOCAL, SOCK_SEQPACKET, AF_LOCAL, outputSockets) < 0)
	{
		perror("socketpair");
		exit(EXIT_FAILURE);
	}
	int childPD;
	// Create the child process.  This will automatically exit when the current
	// process exits.
	int ret = pdfork(&childPD, 0);
	expect(ret >= 0, "pdfork failed: {}", strerror(errno));
	// If we're in the child, start running the child process loop
	if (ret == 0)
	{
		close(outputSockets[0]);
		outputSockets[0] = -1;
		wait_for_files(outputSockets[1]);
	}
	close(outputSockets[1]);
	saveOutput = FileDescriptor{outputSockets[0]};
}

int main(int argc, char **argv)
{
	CLI::App app{"scan-watch file upload"};

	std::string socketPath = "sftp.sock";
	std::string luaPath    = PREFIX "/etc/scan-watch";

	app.add_option("-s,--socket", socketPath, "Path to the socket");
	app.add_option("-l,--lua-scripts", luaPath, "Path to lua match scripts");
	app.add_option("-o,--output-directory", outputPath, "Path to put output");

	try
	{
		app.parse(argc, argv);
	}
	catch (const CLI::ParseError &e)
	{
		return app.exit(e);
	}

	start_child();

	struct sockaddr_un addr;
	unlink(socketPath.c_str());

	int s = socket(PF_LOCAL, SOCK_SEQPACKET, AF_LOCAL);

	sockaddr_un address = {0};
	address.sun_len     = socketPath.size();
	address.sun_family  = AF_UNIX;
	strlcpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path));
	int ret = bind(s, (const struct sockaddr *)&address, sizeof(address));
	expect(ret != -1, "bind to {} failed: {}", socketPath, strerror(errno));
	ret = listen(s, 20);
	expect(ret != -1, "listen on {} failed: {}", socketPath, strerror(errno));

	// Trim trailing slashes
	while ((luaPath.size() > 1) && (luaPath.back() == '/'))
	{
		log("Lua path: {}", luaPath);
		luaPath.pop_back();
	}

	// Preopen directories that we'll need later.
	auto addDirectory = [](const std::string &path) {
		FileDescriptor fd{open(path.c_str(), O_RDONLY | O_DIRECTORY)};
		expect(fd, "Failed to open {}: {}", path, strerror(errno));
		fd.capsicum_limit(CAP_READ, CAP_SEEK, CAP_LOOKUP);
		expect<Status>(fd, "Unable to open directory {}", path);
		preopenedFiles.push_back({path + '/', std::move(fd)});
	};
	addDirectory(tesseractDatapath);
	addDirectory(luaPath);

	log<Verbose>("Dropping privileges");
	cap_enter();

	for (auto const &file :
	     std::filesystem::directory_iterator{std::filesystem::path{luaPath}})
	{
		log<Verbose>("Opening Lua file {}", file.path().string());
		matchers.emplace_back(file.path());
	}

	while (true)
	{
		log<Verbose>("Waiting for connections");
		FileDescriptor pdfs{accept(s, nullptr, nullptr)};
		do
		{
			auto result = receive_file(pdfs);
			if (auto *msg =
			      std::get_if<std::pair<std::string, FileDescriptor>>(&result))
			{
				auto &[fileName, fd] = *msg;
				std::filesystem::path fn{fileName};
				process_page(std::move(fileName), std::move(fd));
			}
			else if (auto *ec = std::get_if<std::error_code>(&result))
			{
				log<Verbose>("Child process error from socket: {}",
				             strerror(ec->value()));
				break;
			}
		} while (true);
	}
}

/// Raw syscall wrappers
extern "C" int __sys_open(const char *, int, mode_t);
extern "C" int __sys_shm_open2(const char *, int, mode_t, int, const char *);

/**
 * Open wrapper.  Once in capability mode, tries to satisfy requests based on
 * pre-opened files, or providing shared memory objects.
 */
extern "C" int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	if ((flags & O_CREAT) == O_CREAT)
	{
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	unsigned inCapMode;
	cap_getmode(&inCapMode);

	// If we're not in cap mode, just do the system call.
	if (!inCapMode)
	{
		return __sys_open(path, flags, mode);
	}

	std::string p{path ? path : ""};

	// If it's a file that's in the fake files list, return it.
	if (fakeFiles.find(p) != fakeFiles.end())
	{
		auto ret = fakeFiles[p].dup();
		// Rewind newly 'opened' file to the start
		lseek(ret, 0, SEEK_SET);
		return ret.take();
	}

	// If it's the current output file, create a shared memory object and
	// stash a copy.
	if (p == currentOutputFileName)
	{
		FileDescriptor fd{memfd_create(p.c_str(), 0)};
		currentOutputFile = fd.dup();
		return fd.take();
	}

	// If it's a temp file, provide a new anonymous shared memory object.
	if (p.starts_with("/tmp/"))
	{
		return memfd_create(p.c_str(), 0);
	}

	// If it's a directory under the preopened trees, use openat to get the
	// right one.
	for (auto &pre : preopenedFiles)
	{
		if (p.starts_with(pre.first))
		{
			return openat(
			  pre.second, p.substr(pre.first.size()).c_str(), flags, mode);
		}
		// If we're trying to open a pre-opened directory, just dup the
		// directory descriptor.
		if (p + '/' == pre.first)
		{
			return dup(pre.second);
		}
	}
	errno = ECAPMODE;
	return -1;
}

/**
 * libc uses _open instead of open to avoid places where open is interposed on.
 * This is unhelpful.
 */
extern "C" int _open(const char *path, int flags, ...)
{
	return open(path, flags);
}

/**
 * The openmp library wants to open temporary files.  It tries using shm_open
 * but sets a name, use anonymous shared memory instead.
 */
extern "C" int shm_open(const char *path, int flags, mode_t mode)
{
	return __sys_shm_open2(SHM_ANON, flags, mode, 0, nullptr);
}
