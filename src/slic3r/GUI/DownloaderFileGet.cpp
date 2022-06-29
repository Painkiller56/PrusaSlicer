#include "DownloaderFileGet.hpp"

#include <thread>
#include <curl/curl.h>
#include <boost/nowide/fstream.hpp>
#include <iostream>

namespace Slic3r {
namespace GUI {

const size_t DOWNLOAD_MAX_CHUNK_SIZE	= 10 * 1024 * 1024;
const size_t DOWNLOAD_SIZE_LIMIT		= 1024 * 1024 * 1024;

std::string FileGet::escape_url(const std::string& unescaped)
{
	std::string ret_val;
	CURL* curl = curl_easy_init();
	if (curl) {
		int decodelen;
		char* decoded = curl_easy_unescape(curl, unescaped.c_str(), unescaped.size(), &decodelen);
		if (decoded) {
			ret_val = std::string(decoded);
			curl_free(decoded);
		}
		curl_easy_cleanup(curl);
	}
	return ret_val;
}
namespace {
unsigned get_current_pid()
{
#ifdef WIN32
	return GetCurrentProcessId();
#else
	return ::getpid();
#endif
}
}

// int = DOWNLOAD ID; string = file path
wxDEFINE_EVENT(EVT_DWNLDR_FILE_COMPLETE, wxCommandEvent);
// int = DOWNLOAD ID; string = error msg
wxDEFINE_EVENT(EVT_DWNLDR_FILE_ERROR, wxCommandEvent);
// int = DOWNLOAD ID; string = progress percent
wxDEFINE_EVENT(EVT_DWNLDR_FILE_PROGRESS, wxCommandEvent);
// int = DOWNLOAD ID; string = name
wxDEFINE_EVENT(EVT_DWNLDR_FILE_NAME_CHANGE, wxCommandEvent);


struct FileGet::priv
{
	const int m_id;
	std::string m_url;
	std::string m_filename;
	std::thread m_io_thread;
	wxEvtHandler* m_evt_handler;
	boost::filesystem::path m_dest_folder;
	boost::filesystem::path m_tmp_path; // path when ongoing download
	std::atomic_bool m_cancel { false };
	std::atomic_bool m_pause  { false };
	size_t m_written { 0 };
	priv(int ID, std::string&& url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder);

	void get_perform();
};

FileGet::priv::priv(int ID, std::string&& url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder)
	: m_id(ID)
	, m_url(std::move(url))
	, m_filename(filename)
	, m_evt_handler(evt_handler)
	, m_dest_folder(dest_folder)
{
}

void FileGet::priv::get_perform()
{
	assert(m_evt_handler);
	assert(!m_url.empty());
	assert(!m_filename.empty());
	assert(boost::filesystem::is_directory(m_dest_folder));

	// open dest file
	if (m_written == 0)
	{
		boost::filesystem::path dest_path = m_dest_folder / m_filename;
		std::string extension = boost::filesystem::extension(dest_path);
		std::string just_filename = m_filename.substr(0, m_filename.size() - extension.size());
		std::string final_filename = just_filename;

		size_t version = 0;
		while (boost::filesystem::exists(m_dest_folder / (final_filename + extension)) || boost::filesystem::exists(m_dest_folder / (final_filename + extension + "." + std::to_string(get_current_pid()) + ".download")))
		{
			++version;
			final_filename = just_filename + "(" + std::to_string(version) + ")";
		}
		m_filename = final_filename + extension;

		m_tmp_path = m_dest_folder / (m_filename + "." + std::to_string(get_current_pid()) + ".download");

		wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_NAME_CHANGE);
		evt->SetString(boost::nowide::widen(m_filename));
		evt->SetInt(m_id);
		m_evt_handler->QueueEvent(evt);
	}
	
	boost::filesystem::path dest_path = m_dest_folder / m_filename;

	std::cout << "dest_path: " << dest_path.string() << std::endl;
	std::cout << "m_tmp_path: " << m_tmp_path.string() << std::endl;
	FILE* file;
	// open file for writting
	if (m_written == 0)
		file = fopen(m_tmp_path.string().c_str(), "wb");
	else 
		file = fopen(m_tmp_path.string().c_str(), "a");

	std:: string range_string = std::to_string(m_written) + "-";

	size_t written_previously = m_written;
	size_t written_this_session = 0;
	Http::get(m_url)
		.size_limit(DOWNLOAD_SIZE_LIMIT) //more?
		.set_range(range_string)
		.on_progress([&](Http::Progress progress, bool& cancel) {
			if (m_cancel) {
				fclose(file);
				// remove canceled file
				std::remove(m_tmp_path.string().c_str());
				m_written = 0;
				cancel = true;
				return;
				// TODO: send canceled event?
			}		
			if (m_pause) {
				fclose(file);
				cancel = true;
				return;
			}
			
			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_PROGRESS);
			/*if (progress.dlnow == 0 && m_written == 0) {
				evt->SetString("0");
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
			} else*/ 
			if (progress.dlnow != 0) {
				if (progress.dlnow - written_this_session > DOWNLOAD_MAX_CHUNK_SIZE || progress.dlnow == progress.dltotal) {
					try
					{
						std::string part_for_write = progress.buffer.substr(written_this_session, progress.dlnow);
						fwrite(part_for_write.c_str(), 1, part_for_write.size(), file);
					}
					catch (const std::exception& e)
					{
						// fclose(file); do it?
						wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
						evt->SetString(e.what());
						evt->SetInt(m_id);
						m_evt_handler->QueueEvent(evt);
						cancel = true;
						return;
					}
					written_this_session = progress.dlnow;
					m_written = written_previously + written_this_session;
				}
				evt->SetString(std::to_string(progress.dlnow * 100 / progress.dltotal));
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
			}
			
		})
		.on_error([&](std::string body, std::string error, unsigned http_status) {
			fclose(file);
			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
			evt->SetString(error);
			evt->SetInt(m_id);
			m_evt_handler->QueueEvent(evt);
		})
		.on_complete([&](std::string body, unsigned /* http_status */) {
			
			size_t body_size = body.size();
			// TODO:
			//if (body_size != expected_size) {
			//	return;
			//}
			try
			{
				/*
				if (m_written < body.size())
				{
					// this code should never be entered. As there should be on_progress call after last bit downloaded.
					std::string part_for_write = body.substr(m_written);
					fwrite(part_for_write.c_str(), 1, part_for_write.size(), file);
				}
				*/
				fclose(file);
				boost::filesystem::rename(m_tmp_path, dest_path);
			}
			catch (const std::exception& e)
			{
				//TODO: report?
				//error_message = GUI::format("Failed to write and move %1% to %2%", tmp_path, dest_path);
				wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_ERROR);
				evt->SetString("Failed to write and move.");
				evt->SetInt(m_id);
				m_evt_handler->QueueEvent(evt);
				return;
			}

			wxCommandEvent* evt = new wxCommandEvent(EVT_DWNLDR_FILE_COMPLETE);
			evt->SetString(dest_path.string());
			evt->SetInt(m_id);
			m_evt_handler->QueueEvent(evt);
		})
		.perform_sync();

}

FileGet::FileGet(int ID, std::string url, const std::string& filename, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder)
	: p(new priv(ID, std::move(url), filename, evt_handler, dest_folder))
{}

FileGet::FileGet(FileGet&& other) : p(std::move(other.p)) {}

FileGet::~FileGet()
{
	if (p && p->m_io_thread.joinable()) {
		p->m_cancel = true;
		p->m_io_thread.join();
	}
}

void FileGet::get()
{
	assert(p);
	if (p->m_io_thread.joinable()) {
			// This will stop transfers being done by the thread, if any.
			// Cancelling takes some time, but should complete soon enough.
			p->m_cancel = true;
			p->m_io_thread.join();
	}
	p->m_cancel = false;
	p->m_pause = false;
	p->m_io_thread = std::thread([this]() {
		p->get_perform();
		});
}

void FileGet::cancel()
{
	if(p){
		p->m_cancel = true;
	}
}

void FileGet::pause()
{
	if (p) {
		p->m_pause = true;
	}
}
void FileGet::resume()
{
	assert(p);
	if (p->m_io_thread.joinable()) {
		// This will stop transfers being done by the thread, if any.
		// Cancelling takes some time, but should complete soon enough.
		p->m_cancel = true;
		p->m_io_thread.join();
	}
	p->m_cancel = false;
	p->m_pause = false;
	p->m_io_thread = std::thread([this]() {
		p->get_perform();
		});
}
}
}