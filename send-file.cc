#include "helpers.hh"
#include <CLI/CLI.hpp>
#include <fcntl.h>
#include <iostream>

int main(int argc, char **argv)
{
	CLI::App app{"scan-watch file upload"};
	app.allow_extras();
	std::string socketPath = "sftp.sock";
	app.add_option("-s,--socket", socketPath, "Path to the socket");
	try
	{
		app.parse(argc, argv);
	}
	catch (const CLI::ParseError &e)
	{
		return app.exit(e);
	}

	auto resultSocket = connect_socket(socketPath.c_str());
	if (!resultSocket)
	{
		perror("Failed to open socket");
		exit(EXIT_FAILURE);
	}
	for (auto arg : app.remaining())
	{
		FileDescriptor fd{open(arg.c_str(), O_RDONLY)};
		fd.capsicum_limit(CAP_READ, CAP_SEEK, CAP_FSTAT, CAP_FCNTL);
		send_file(arg, fd, resultSocket);
	}
}
