#include "streams/file_stream.h"
#include "../LibretroRT_Tools/StringConverter.h"
#include "../LibretroRT_Tools/NativeBuffer.h"
#include "../LibretroRT/libretro_extra.h"

#include <string>
#include <sstream>
#include <collection.h>
#include <ppltasks.h>

using namespace std;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

const size_t StaticBufferLen = 4096;

char IntermediateStringBuffer[StaticBufferLen];
wchar_t IntermediateWStringBuffer[StaticBufferLen];

retro_extra_open_file_t OpenFileStreamViaFrontend = nullptr;
retro_extra_close_file_t CloseFileStreamViaFrontend = nullptr;

void retro_extra_set_open_file(retro_extra_open_file_t cb)
{
	OpenFileStreamViaFrontend = cb;
}

void retro_extra_set_close_file(retro_extra_close_file_t cb)
{
	CloseFileStreamViaFrontend = cb;
}

struct RFILE
{
	string Path;
	string FileType;
	IRandomAccessStream^ Stream;

	RFILE() :
		Path(nullptr),
		FileType(nullptr),
		Stream(nullptr)
	{
	}

	RFILE(string path, IRandomAccessStream^ stream) :
		Path(path),
		Stream(stream)
	{
		auto extStartIx = path.find_last_of('.');
		FileType = path.substr(extStartIx);
	}
};

long long int filestream_get_size(RFILE *stream)
{
	return stream->Stream->Size;
}

const char *filestream_get_ext(RFILE *stream)
{
	return stream->FileType.c_str();
}

RFILE *filestream_open(const char *path, unsigned mode, ssize_t len)
{
	string pathStr(path);

	if (OpenFileStreamViaFrontend == nullptr)
	{
		return nullptr;
	}

	auto convertedPath = LibretroRT_Tools::StringConverter::CPPToPlatformString(pathStr);
	mode = mode & 0x0f;
	auto accessMode = (mode == RFILE_MODE_READ || mode == RFILE_MODE_READ_TEXT) ? FileAccessMode::Read : FileAccessMode::ReadWrite;

	auto stream = OpenFileStreamViaFrontend(convertedPath, accessMode);
	if (stream == nullptr)
	{
		return nullptr;
	}

	auto output = new RFILE(pathStr, stream);
	return output;
}

ssize_t filestream_seek(RFILE *stream, ssize_t offset, int whence)
{
	auto winStream = stream->Stream;
	switch (whence)
	{
	case SEEK_SET:
		winStream->Seek(offset);
		break;
	case SEEK_CUR:
		winStream->Seek(winStream->Position + offset);
		break;
	case SEEK_END:
		winStream->Seek(winStream->Size - offset);
		break;
	}

	return 0;
}

ssize_t filestream_read(RFILE *stream, void *data, size_t len)
{
	auto winStream = stream->Stream;
	auto remaining = winStream->Size - winStream->Position;
	auto output = min(len, remaining);

	auto buffer = LibretroRT_Tools::CreateNativeBuffer(data, len);
	concurrency::create_task(winStream->ReadAsync(buffer, output, InputStreamOptions::None)).wait();
	return output;
}

ssize_t filestream_write(RFILE *stream, const void *data, size_t len)
{
	auto dataArray = Platform::ArrayReference<unsigned char>((unsigned char*)data, len);
	auto writer = ref new DataWriter(stream->Stream);
	writer->WriteBytes(dataArray);
	concurrency::create_task(stream->Stream->FlushAsync()).wait();
	writer->DetachStream();
	return len;
}

ssize_t filestream_tell(RFILE *stream)
{
	return stream->Stream->Position;
}

void filestream_rewind(RFILE *stream)
{
	stream->Stream->Seek(0);
}

int filestream_close(RFILE *stream)
{
	CloseFileStreamViaFrontend(stream->Stream);
	delete stream;
	return 0;
}

int filestream_read_file(const char *path, void **buf, ssize_t *len)
{
	ssize_t ret = 0;
	ssize_t content_buf_size = 0;
	void *content_buf = NULL;
	RFILE *file = filestream_open(path, RFILE_MODE_READ, -1);

	if (!file)
	{
		goto error;
	}

	if (filestream_seek(file, 0, SEEK_END) != 0)
		goto error;

	content_buf_size = filestream_tell(file);
	if (content_buf_size < 0)
		goto error;

	filestream_rewind(file);

	content_buf = malloc(content_buf_size + 1);

	if (!content_buf)
		goto error;

	ret = filestream_read(file, content_buf, content_buf_size);
	if (ret < 0)
	{
		goto error;
	}

	filestream_close(file);

	*buf = content_buf;

	/* Allow for easy reading of strings to be safe.
	* Will only work with sane character formatting (Unix). */
	((char*)content_buf)[ret] = '\0';

	if (len)
		*len = ret;

	return 1;

error:
	if (file)
		filestream_close(file);
	if (content_buf)
		free(content_buf);
	if (len)
		*len = -1;
	*buf = NULL;
	return 0;
}

char *filestream_gets(RFILE *stream, char *s, size_t len)
{
	auto winStream = stream->Stream;
	auto initialPos = winStream->Position;
	if (initialPos == winStream->Size)
	{
		return NULL;
	}

	auto reader = ref new DataReader(winStream);
	len = concurrency::create_task(reader->LoadAsync(len)).get();
	if (len < 1)
	{
		return NULL;
	}

	auto winString = reader->ReadString(len);
	reader->DetachStream();

	auto converted = LibretroRT_Tools::StringConverter::PlatformToCPPString(winString);
	std::istringstream stringStream(converted);
	std::string line;
	std::getline(stringStream, line);
	strcpy_s(s, len, line.c_str());

	auto index = converted.find('\n');
	winStream->Seek(min(winStream->Size, initialPos + index + 1));
	return s;
}

//char *filestream_getline(RFILE *stream);

int filestream_getc(RFILE *stream)
{
	auto reader = ref new DataReader(stream->Stream);
	concurrency::create_task(reader->LoadAsync(1)).wait();
	auto output = reader->ReadByte();
	reader->DetachStream();
	return output;
}

int filestream_eof(RFILE *stream)
{
	size_t current_position = filestream_tell(stream);
	size_t end_position = filestream_seek(stream, 0, SEEK_END);

	filestream_seek(stream, current_position, SEEK_SET);

	if (current_position >= end_position)
		return 1;
	return 0;
}

bool filestream_write_file(const char *path, const void *data, ssize_t size)
{
	ssize_t ret = 0;
	RFILE *file = filestream_open(path, RFILE_MODE_WRITE, -1);
	if (!file)
		return false;

	ret = filestream_write(file, data, size);
	filestream_close(file);

	if (ret != size)
		return false;

	return true;
}

int filestream_putc(RFILE *stream, int c)
{
	auto writer = ref new DataWriter(stream->Stream);
	writer->WriteByte(c);
	concurrency::create_task(stream->Stream->FlushAsync()).wait();
	writer->DetachStream();
	return c;
}

int filestream_flush(RFILE *stream)
{
	concurrency::create_task(stream->Stream->FlushAsync()).wait();
	return 0;
}