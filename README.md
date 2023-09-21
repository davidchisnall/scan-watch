Scan Watch
==========

This program is designed to be used with a scanner that can upload JPEGs via SFTP.
It OCRs the image and then uses (user-provided) Lua scripts to assemble pages
into documents and give them names.

I wrote this because I wanted it to exist.
In its current form, it may or may not be useful to other people.

It currently runs only on FreeBSD.
Patches are welcome to support other platforms.
I have not submitted it to ports because I have not seen any evidence that anyone else wants to use it.
Please open issues if it would be useful for you to have packages.

Building and installing (FreeBSD)
---------------------------------

Install dependencies (other versions of Lua should also work):

```sh
# pkg ins tesseract sol2 cli11 magic_enum lua54
```

Of these, only tesseract and lua are required to run.

Build the package:

```sh
$ mkdir -p ${build_directory}
$ cd ${build_directory}
$ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ${source_directory}
$ ninja package
```

This will build a package file that can be installed with `pkg add`.

Security architecture
---------------------

Scan Watch does a token amount of privilege separation.
The main threat model is that the scanner is compromised (because printers and scanners are the things that penetration testers love the most) and tries to compromise the rest of the system.

Uploads run with a custom SFTP server.
OpenSSH handles all key exchange and then runs `scan-watch-sftp-server`.
Before reading any untrusted input, this connects to a socket and enters capability mode.
At this point, it has no access to anything other than the standard streams (connected to the client) and the output socket.
An attacker compromising it has no ability to do anything other than send malicious data to the socket.
This program implements a subset of SFTP, just necessary for receiving uploaded files.
The files are read into anonymous shared memory objects and passed over the socket.

The main processing is done by the `scan-watch-ocr` program.
This opens the Lua scripts directory and the Tesseract data directory read only and creates the socket that the SFTP server will talk to.
It then enters capability mode.

The `scan-watch-ocr` program also forks a child process at start that has write access to the output directory (and also runs in capability mode).
This receives files over the socket and writes them to the raw inputs subdirectory if they are .jpg files or the processed output subdirectory if they are .pdf files.
It does not inspect the contents of the files that it receives.
It always uses `O_EXCL` when opening files, and so cannot (in the absence of an arbitrary-code-execution vulnerability) do anything other than add new files and, even in the presence of a complete compromise, cannot write outside of the designated output directory.

Before reading from the socket, the main `scan-watch-ocr` process has write access to only the output socket to the child.
If an attacker compromises the printer and sends malicious files that trigger a hypothetical arbitrary-code-execution vulnerability in Leptonica's JPEG parsing, the only thing that the main process can do is send malicious messages to the child process.

Both sockets use sequential packets where each packet contains a file name and a file descriptor for the file.
Any message that has a zero length or does not contain a file descriptor is ignored.
The file name is stored in a `std::string`.
It is decomposed in the final step of the pipeline to identify where to put the output, but is not otherwise parsed and is treated as an opaque token.

Automatic submission
--------------------

Scan Watch includes a cut-down SFTP implementation (`scan-watch-sftp-server`) that opens a UNIX domain socket and drops all other privileges.
This server accepts uploaded files and passes them over the socket.
This ensures that a compromise to the printer cannot delete files or leak files that have already been uploaded.

This is enabled by adding the following to the end of `sshd_config`:

```
Match User {scanner upload user}
	ForceCommand {path to scan-watch-sftp-server}
```

This expects to find the socket `sftp.sock` in the home directory of the upload user.
Setting this user's home directory to `/var/run/scan_watch` will avoid further configuration.


Configuring the server
----------------------

The package installs an rc script that can be used to run the dæmon.
All configuration options for it should be set in either `/etc/rc.conf` or `/etc/rc.conf.d/scan_watch`.

The following options can be set:

 - `scan_watch_enable` set to `YES` to enable the dæmon.
 - `scan_watch_username` is the user to run as.
   By default, this is `scanner`.
   This should be the same user as the SFTP upload user and must have write permission to the output directory.
 - `scan_watch_socket_path` is the location of the socket.
   This defaults to `/var/run/scan_watch/sftp.sock`, which should be the home directory of the upload user.
 - `scan_watch_script_dir` is the location of the scripts to run.
   This defaults to `${PREFIX}/etc/scan_watch`.
 - `scan_watch_output_dir` is the output location.
   This defaults to `/share/scans`, which is almost certainly a silly default.
   The output directory *must* contain two subdirectories, one called `output` and one called `raw_inputs`.
   This is also a silly requirement and, in the future, the two output locations will be separately specified.

With these set, you can use `service scan_watch {start,stop}` to start and stop the service.
Note that the dæmon does not yet shut down gracefully, so you need to wait for it to process all inputs before stopping it.

Scripting
---------

Each matcher script exports two functions:

The `test` function is used to determine whether a particular page looks like it should be recognised by the current matcher.
This function returns a match probability (0–100).
The matcher that returns the highest probability will be used to process the page.
The `test` function is called on every matches and so should be fast, exiting early if it determines a high probability.

The `match` function determines how to process the page.
This can be slower and is called only when a matcher has been chosen.

The `match` function returns a table with some of the following fields:

 - `discard` is a boolean value that is set if this page should be ignored.
 - `page` is the page number for this page.
 - `page_count` is the page count for this document.
 - `title` is the title that will be used for the document.
 - `finish` is a boolean value that is set if this page should be the last in a document.

If `count` is provided by any page in a set then the document will be assumed to be completed once this many pages are provided.

If at least one of the pages in a document has a title then this title will be used for the output file.
If multiple pages provide identical titles then this title will be used.
If multiple pages provide different titles then they will be concatenated to give the final output title.

If `discard` is set then the page will not be added to a document and will be dropped.
If `finish` is set then the page is assumed to be the last in the document and the PDF will be generated.

Both functions are passed an table that you can iterate over to see each text run that the OCR has provided.
Each element contains the following fields:

 - `text`:The text for this fragment.
 - `confidence`: The confidence that the OCR engine places in this recognition.
 - `isBold`: Is this bold text?
 - `isItalic`: Is this italic text?
 - `isUnderlined`: Is this underlined text?
 - `isMonospace`: Is this monospaced text?
 - `isSerif`: Is this serif text?
 - `isSmallCap`: Is this small-caps text?
 - `pointSize`: The point size of the recognised text.


