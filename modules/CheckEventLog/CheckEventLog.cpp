/**************************************************************************
*   Copyright (C) 2004-2007 by Michael Medin <michael@medin.name>         *
*                                                                         *
*   This code is part of NSClient++ - http://trac.nakednuns.org/nscp      *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
***************************************************************************/

#include "stdafx.h"

#include <boost/foreach.hpp>
#include <boost/program_options.hpp>


#include "CheckEventLog.h"

#include <time.h>
#include <error.hpp>
#include <map>
#include <vector>

#include <boost/bind.hpp>
#include <boost/assign.hpp>
#include <boost/program_options.hpp>

#include "filter.hpp"

#include <parsers/filter/cli_helper.hpp>

#include <nscapi/nscapi_protobuf_functions.hpp>
#include <nscapi/nscapi_core_helper.hpp>
#include <nscapi/nscapi_program_options.hpp>
#include <nscapi/nscapi_protobuf_functions.hpp>

#include <settings/client/settings_client.hpp>

namespace sh = nscapi::settings_helper;
namespace po = boost::program_options;

#include "simple_registry.hpp"
#include "eventlog_record.hpp"
#include "real_time_thread.hpp"

struct parse_exception {
	parse_exception(std::wstring) {}
};


bool CheckEventLog::loadModuleEx(std::string alias, NSCAPI::moduleLoadMode mode) {
	sh::settings_registry settings(get_settings_proxy());
	settings.set_alias(alias, "eventlog");
		
	thread_.reset(new real_time_thread());
	if (!thread_) {
		NSC_LOG_ERROR_STD("Failed to create thread container");
		return false;
	}
	thread_->filters_path_ = settings.alias().get_settings_path("real-time/filters");


	settings.alias().add_path_to_settings()
		("EVENT LOG SECTION", "Section for the EventLog Checker (CheckEventLog.dll).")

		("real-time", "CONFIGURE REALTIME CHECKING", "A set of options to configure the real time checks")

		("real-time/filters", sh::fun_values_path(boost::bind(&real_time_thread::add_realtime_filter, thread_, get_settings_proxy(), _1, _2)),  
		"REALTIME FILTERS", "A set of filters to use in real-time mode")
		;

	settings.alias().add_key_to_settings()
		("debug", sh::bool_key(&debug_, false),
		"DEBUG", "Log more information when filtering (useful to detect issues with filters) not useful in production as it is a bit of a resource hog.")

		("lookup names", sh::bool_key(&lookup_names_, true),
		"LOOKUP NAMES", "Lookup the names of eventlog files")

		("syntax", sh::string_key(&syntax_),
		"SYNTAX", "Set this to use a specific syntax string for all commands (that don't specify one).")

		("buffer size", sh::int_key(&buffer_length_, 128*1024),
		"BUFFER_SIZE", "The size of the buffer to use when getting messages this affects the speed and maximum size of messages you can recieve.")

		;

	settings.alias().add_key_to_settings("real-time")

		("enabled", sh::bool_fun_key<bool>(boost::bind(&real_time_thread::set_enabled, thread_, _1), false),
		"REAL TIME CHECKING", "Spawns a background thread which detects issues and reports them back instantly.")

		("startup age", sh::string_fun_key<std::string>(boost::bind(&real_time_thread::set_start_age, thread_, _1), "30m"),
		"STARTUP AGE", "The initial age to scan when starting NSClient++")

		("log", sh::string_key(&thread_->logs_ , "application,system"),
		"LOGS TO CHECK", "Comma separated list of logs to check")

		("debug", sh::bool_key(&thread_->debug_, false),
		"DEBUG", "Log missed records (useful to detect issues with filters) not useful in production as it is a bit of a resource hog.")

		;

	settings.register_all();
	settings.notify();

	if (mode == NSCAPI::normalStart) {
		if (!thread_->start())
			NSC_LOG_ERROR_STD("Failed to start collection thread");
	}
	return true;
}
bool CheckEventLog::unloadModule() {
	if (!thread_->stop())
		NSC_LOG_ERROR_STD("Failed to start collection thread");
	return true;
}

class uniq_eventlog_record {
	DWORD ID;
	WORD type;
	WORD category;
public:
	std::string message;
	uniq_eventlog_record(EVENTLOGRECORD *pevlr) : ID(pevlr->EventID&0xffff), type(pevlr->EventType), category(pevlr->EventCategory) {}
	bool operator< (const uniq_eventlog_record &other) const { 
		return (ID < other.ID) || ((ID==other.ID)&&(type < other.type)) || (ID==other.ID&&type==other.type)&&(category < other.category);
	}
	std::wstring to_string() const {
		return _T("id=") + strEx::itos(ID) + _T("type=") + strEx::itos(type) + _T("category=") + strEx::itos(category);
	}
};
typedef std::map<uniq_eventlog_record,unsigned int> uniq_eventlog_map;

struct event_log_buffer {
	BYTE *bBuffer;
	DWORD bufferSize_;
	event_log_buffer(DWORD bufferSize) : bufferSize_(bufferSize) {
		bBuffer = new BYTE[bufferSize+10];
	}
	~event_log_buffer() {
		delete [] bBuffer;
	}
	EVENTLOGRECORD* getBufferUnsafe() {
		return reinterpret_cast<EVENTLOGRECORD*>(bBuffer);
	}
	DWORD getBufferSize() {
		return bufferSize_;
	}
};

inline std::time_t to_time_t(boost::posix_time::ptime t) { 
	if( t == boost::date_time::neg_infin ) 
		return 0; 
	else if( t == boost::date_time::pos_infin ) 
		return LONG_MAX; 
	boost::posix_time::ptime start(boost::gregorian::date(1970,1,1)); 
	return (t-start).total_seconds(); 
} 

inline long long parse_time(std::string time) {
	long long now = to_time_t(boost::posix_time::second_clock::universal_time());
	std::string::size_type p = time.find_first_not_of("-0123456789");
	if (p == std::string::npos)
		return now + boost::lexical_cast<long long>(time);
	long long value = boost::lexical_cast<long long>(time.substr(0, p));
	if ( (time[p] == 's') || (time[p] == 'S') )
		return now + value;
	else if ( (time[p] == 'm') || (time[p] == 'M') )
		return now + (value * 60);
	else if ( (time[p] == 'h') || (time[p] == 'H') )
		return now + (value * 60 * 60);
	else if ( (time[p] == 'd') || (time[p] == 'D') )
		return now + (value * 24 * 60 * 60);
	else if ( (time[p] == 'w') || (time[p] == 'W') )
		return now + (value * 7 * 24 * 60 * 60);
	return now + value;
}


void CheckEventLog::check_eventlog(const Plugin::QueryRequestMessage::Request &request, Plugin::QueryResponseMessage::Response *response) {
	typedef eventlog_filter::filter filter_type;
	modern_filter::data_container data;
	modern_filter::cli_helper<filter_type> filter_helper(request, response, data);
	std::vector<std::string> file_list;
	std::string files_string;
	std::string mode;
	std::string scan_range;

	filter_type filter;
	filter_helper.add_options(filter.get_filter_syntax());
	filter_helper.add_syntax("${file}: ${count} (${problem_list})", filter.get_format_syntax(), "${file} ${source} (${message})", "${file}_${source}");
	filter_helper.get_desc().add_options()
		("file", po::value<std::vector<std::string> >(&file_list),	"File to read (can be specified multiple times to check multiple files.\nNotice that specifying multiple files will create an aggregate set you will not check each file individually."
		"In other words if one file contains an error the entire check will result in error.")
		("scan-range", po::value<std::string>(&scan_range), "Date range to scan.\nThis is the approximate dates to search through this speeds up searching a lot but there is no guarantee messages are ordered.")
		;
	if (!filter_helper.parse_options())
		return;

	if (filter_helper.empty()) {
		data.filter_string = "level not in ('info', 'success', 'auditSuccess')";
		filter_helper.set_default("count > 0", "count > 5");
		scan_range = "-24h";
	}
	if (file_list.empty()) {
		file_list.push_back("Application");
		file_list.push_back("System");
	}

	if (!filter_helper.build_filter(filter))
		return;

	event_log_buffer buffer(buffer_length_);

// 	desc.add_options()
// 		("unique", po::bool_switch(&unique), "Only return one of each message (based on message id and source).")
// 		;

	BOOST_FOREACH(const std::string &file, file_list) {
		std::string name = file;
		if (lookup_names_) {
			name = eventlog_wrapper::find_eventlog_name(name);
			if (file != name) {
				NSC_DEBUG_MSG_STD("Opening alternative log: " + utf8::cvt<std::string>(name));
			}
		}
		HANDLE hLog = OpenEventLog(NULL, utf8::cvt<std::wstring>(name).c_str());
		if (hLog == NULL)
			return nscapi::protobuf::functions::set_response_bad(*response, "Could not open the '" + utf8::cvt<std::string>(name) + "' event log: "  + utf8::cvt<std::string>(error::lookup::last_error()));
		uniq_eventlog_map uniq_records;
		long long stop_date;
		enum direction_type {
			direction_none, direction_forwards, direction_backwards

		};
		direction_type direction = direction_none;
		DWORD flags = EVENTLOG_SEQUENTIAL_READ;
		if ((scan_range.size() > 0) && (scan_range[0] == L'-')) {
			direction = direction_backwards;
			flags|=EVENTLOG_BACKWARDS_READ;
			stop_date = parse_time(scan_range);
		} else if (scan_range.size() > 0) {
			direction = direction_forwards;
			flags|=EVENTLOG_FORWARDS_READ;
			stop_date = parse_time(scan_range);
		} else {
			flags|=EVENTLOG_FORWARDS_READ;
		}

		DWORD dwRead, dwNeeded;
		bool is_scanning = true;
		bool buffer_error_reported = false;
		while (is_scanning) {

			BOOL bStatus = ReadEventLog(hLog, flags,
				0, buffer.getBufferUnsafe(), buffer.getBufferSize(), &dwRead, &dwNeeded);
			if (bStatus == FALSE) {
				DWORD err = GetLastError();
				if (err == ERROR_INSUFFICIENT_BUFFER) {
					if (!buffer_error_reported) {
						NSC_LOG_ERROR_STD("EvenlogBuffer is too small change the value of buffer_length=" + strEx::s::xtos(dwNeeded+1));
						buffer_error_reported = true;
					}
				} else if (err == ERROR_HANDLE_EOF) {
					is_scanning = false;
					break;
				} else {
					std::string error_msg = error::lookup::last_error(err);
					CloseEventLog(hLog);
					return nscapi::protobuf::functions::set_response_bad(*response, "Failed to read from event log: " + error_msg);
				}
			}
			__time64_t ltime;
			_time64(&ltime);

			EVENTLOGRECORD *pevlr = buffer.getBufferUnsafe(); 
			while (dwRead > 0) { 
				EventLogRecord record(file, pevlr, ltime);
				if (direction == direction_backwards && record.written() < stop_date) {
					is_scanning = false;
					break;
				}
				if (direction == direction_forwards && record.written() > stop_date) {
					is_scanning = false;
					break;
				}
				boost::tuple<bool,bool> ret = filter.match(boost::make_shared<eventlog_filter::filter_obj>(record));
				if (ret.get<1>()) {
					break;
				}
// 				bool match = impl->match(arg);
// 				if (match&&unique) {
// 					match = false;
// 					uniq_eventlog_record uniq_record = pevlr;
// 					uniq_eventlog_map::iterator it = uniq_records.find(uniq_record);
// 					if (it != uniq_records.end()) {
// 						(*it).second ++;
// 					}
// 					else {
// 						uniq_record.message = record.render(fargs->bShowDescriptions, fargs->syntax);
// 						uniq_records[uniq_record] = 1;
// 					}
// 					hit_count++;
// 				} else if (match) {
// 					if (!fargs->syntax.empty()) {
// 						strEx::append_list(message, record.render(fargs->bShowDescriptions, fargs->syntax));
// 					}
// 					hit_count++;
// 				}
				dwRead -= pevlr->Length; 
				pevlr = reinterpret_cast<EVENTLOGRECORD*>((LPBYTE)pevlr + pevlr->Length); 
			} 
		}
		CloseEventLog(hLog);
// 		BOOST_FOREACH(const uniq_eventlog_map::value_type &v, uniq_records) {
// 			std::string msg = v.first.message;
// 			strEx::replace(msg, "%count%", strEx::s::xtos(v.second));
// 			strEx::append_list(message, msg);
// 		}
	}
	modern_filter::perf_writer writer(response);
	filter_helper.post_process(filter, &writer);
}

NSCAPI::nagiosReturn CheckEventLog::commandLineExec(const std::string &command, const std::list<std::string> &arguments, std::string &result) {
	if (command == "insert-message" || command == "insert" || command.empty()) {
		bool ok = insert_eventlog(arguments, result);
		return ok?NSCAPI::isSuccess:NSCAPI::hasFailed;
	} else if (command == "help") {
		std::list<std::string> args;
		args.push_back("--help");
		insert_eventlog(args, result);
		return NSCAPI::isSuccess;
	}
	return NSCAPI::returnIgnored;
}



NSCAPI::nagiosReturn CheckEventLog::insert_eventlog(const std::list<std::string> &arguments, std::string &message) {
	try {
		namespace po = boost::program_options;

		bool help = false;
		std::string type, severity;
		std::wstring source_name;
		std::vector<std::wstring> strings;
		WORD wEventID = 0, category = 0, customer = 0;
		WORD facility = 0;
		po::options_description desc("Allowed options");
		desc.add_options()
			("help,h", po::bool_switch(&help), "Show help screen")
			("source,s", po::wvalue<std::wstring>(&source_name)->default_value(_T("Application Error")), "source to use")
			("type,t", po::value<std::string>(&type), "Event type")
			("level,l", po::value<std::string>(&type), "Event level (type)")
			("facility,f", po::value<WORD>(&facility), "Facility/Qualifier")
			("qualifier,q", po::value<WORD>(&facility), "Facility/Qualifier")
			("severity", po::value<std::string>(&severity), "Event severity")
			("category,c", po::value<WORD>(&category), "Event category")
			("customer", po::value<WORD>(&customer), "Customer bit 0,1")
			("arguments,a", po::wvalue<std::vector<std::wstring> >(&strings), "Message arguments (strings)")
			("id,i", po::value<WORD>(&wEventID), "Event ID")
			;

		boost::program_options::variables_map vm;

		std::vector<std::string> vargs(arguments.begin(), arguments.end());
		po::parsed_options parsed = po::basic_command_line_parser<char>(vargs).options(desc).run();
		po::store(parsed, vm);
		po::notify(vm);

		if (help || arguments.empty()) {
			std::stringstream ss;
			ss << "CheckEventLog Command line syntax:" << std::endl;
			ss << desc;
			message = ss.str();
			return NSCAPI::isSuccess;
		} else {
			event_source source(source_name);
			WORD wType = EventLogRecord::translateType(type);
			WORD wSeverity = EventLogRecord::translateSeverity(severity);
			DWORD tID = (wEventID&0xffff) | ((facility&0xfff)<<16) | ((customer&0x1)<<29) | ((wSeverity&0x3)<<30);
			LPCWSTR *string_data = new LPCWSTR[strings.size()];
			int i=0;
			// TODO: FIxme this is broken!
			BOOST_FOREACH(const std::wstring &s, strings) {
				string_data[i++] = s.c_str();
			}

			if (!ReportEvent(source, wType, category, tID, NULL, static_cast<WORD>(strings.size()), 0, string_data, NULL)) {
				message = "Could not report the event";
				return NSCAPI::hasFailed;
			} else {
				message = "Message reported successfully";
			}
			delete [] string_data;
		}
	} catch (const std::exception &e) {
		NSC_LOG_ERROR_EXR("Failed to parse command line: ", e);
	}
	return NSCAPI::returnIgnored;
}
