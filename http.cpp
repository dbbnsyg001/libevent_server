#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <iostream>
#include <algorithm>
#include <string>
#include <sys/socket.h>
#include "http.h"
#include "log.h"

using namespace std;

int judge_dir_or_file(string file);
map<string, string> Http::type_map;

void Http::insert_map(const string& key, const string& v)
{
	type_map[key] = "Content-type: " + v + "\r\n";
}

void Http::init_map()
{
	if(type_map.empty())
	{
		insert_map(".html", "text/html");
		insert_map(".jpg", "image/jpeg");
		insert_map(".png", "image/png");
	}
}

string Http::get_type(const string& key)
{
	string res;
	map<string, string>::iterator iter = type_map.find(key);
	if(iter != type_map.end())
	{
		res = iter->second;	
	}
	return res;
}

void Http::read_cb(bufferevent *bev, void *ctx)
{
	DEBUG_LOG("read");
	Http* http = (Http*)ctx;

	if(http->bev == bev)
	{
		while(http->loop());	
	}
	else if(http->bev_cgi == bev)
	{
		evbuffer_add_buffer(bufferevent_get_output(http->bev), bufferevent_get_input(bev));
	}
	else
	{
		DEBUG_LOG("error read_cb");	
	}
}

void Http::write_cb(bufferevent *bev, void *ctx)
{
	Http* http = (Http*)ctx;
	evbuffer *output = bufferevent_get_output(bev);
	if(http->get_all_send() && 
			evbuffer_get_length(output) == 0)
	{
		DEBUG_LOG("empty");
		Http::release(&http);
	}
}

void Http::event_cb(bufferevent *bev, short events, void *ctx)
{
	if (events & BEV_EVENT_ERROR)
	{
		int err = EVUTIL_SOCKET_ERROR();

		if (err)
		{
			DEBUG_LOG("Socket error: %s\n", evutil_socket_error_to_string(err));
		}
	}

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
	{
		Http* http = (Http*) ctx;

		if(http->bev_cgi == bev)
		{
			evbuffer *output = bufferevent_get_output(http->bev);
			if(evbuffer_get_length(output) == 0)
			{
				DEBUG_LOG("release http");
				Http::release(&http);	
			}
			else
			{
				DEBUG_LOG("release unix");
				http->set_all_send(true);
				bufferevent_free(http->bev_cgi);
				http->bev_cgi = NULL;
			}	
		}
		else if(http->bev == bev)
		{
			DEBUG_LOG("release http");
			Http::release(&http);
		}
		else
		{
			DEBUG_LOG("error event_cb");	
		}
	}
}

Http* Http::create(event_base* base, evutil_socket_t fd)
{
	Http* http	= new Http(base, fd);
	return http;
}

void Http::release(Http** p_http)
{
	delete *p_http;
	*p_http = NULL;
}

Http::Http(event_base* base, evutil_socket_t fd): status(REQUEST_LINE), 
	all_send(false), bev_cgi(NULL)
{
	Http::init_map();
	bev = bufferevent_socket_new(base, fd, 
			BEV_OPT_CLOSE_ON_FREE /*| BEV_OPT_THREADSAFE*/);

	if(bev == NULL)
	{
		DEBUG_LOG("get bufferevent error!");
		return;
	}
}

Http::~Http()
{
	if(bev)
	{
		bufferevent_free(bev);
		bev = NULL;
	}

	if(bev_cgi)
	{
		bufferevent_free(bev_cgi);
		bev_cgi = NULL;
	}
	DEBUG_LOG("~Http");
}

bool Http::get_all_send()
{
	return all_send;
}

void Http::set_all_send(bool v)
{
	all_send = v;	
}

void Http::run(void* arg)
{
	bufferevent_setcb(bev, read_cb, write_cb, event_cb, arg);
	bufferevent_enable(bev, EV_READ);
}

char* Http::get_word(char* line, string& res)
{
	for(; *line && *line == ' '; ++line);

	for(; *line && *line != ' '; ++line)
	{
		res += *line;
	}
	return line;
}

bool Http::parse_request_line()
{
	assert(status == REQUEST_LINE);
	evbuffer *input = bufferevent_get_input(bev);

	char* line;
	size_t len;
	line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF);
	if(line) 
	{
		char* p = line;
		p = get_word(p, method);
		p = get_word(p, path);
		p = get_word(p, version);
		DEBUG_LOG("method: %s", method.c_str());
		DEBUG_LOG("path: %s", path.c_str());
		DEBUG_LOG("version: %s", version.c_str());
		
		if(method.empty() || path.empty() || version.empty())
		{
			DEBUG_LOG("%s", line);
			status = ERROR_STATUS;
			free(line);	
			return false;
		}
		
		transform(method.begin(), method.end(), method.begin(), ::toupper);  
		free(line);	
		status = HEADER;
		return true;
	}

	DEBUG_LOG("%s", line);
	status = ERROR_STATUS;
	free(line);	
	return false;
}

bool Http::parse_header()
{
	assert(status == HEADER);
	evbuffer *input = bufferevent_get_input(bev);

	char* line;
	size_t len;
	line = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF);
	if(line == NULL) 
	{
		return false;
	}

	char* p = line;
	for(; *p && *p == ' '; ++p);		

	string key, value;
	for(; *p && *p != ' ' && *p != ':'; ++p)
	{
		key += *p;
	}

	if(key.empty())
	{
		status = MSG_BODY;
		free(line);
		return true;	
	}

	for(; *p && (*p == ' ' || *p == ':'); ++p);		
	for(; *p && *p != ' ' && *p != ':'; ++p)
	{
		value += *p;
	}

	header_map.insert(make_pair(key, value));
	DEBUG_LOG("%s : %s", key.c_str(), value.c_str());

	free(line);
	return true;
}

bool Http::parse_msg_body()
{
	assert(status == MSG_BODY);
	evbuffer *input = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(input);
	if(len == 0)
	{
		DEBUG_LOG("msg_body is empty");
	}
	else
	{
		msg_body = (char*)evbuffer_pullup(input, len);
		DEBUG_LOG("msg_body:%s", msg_body.c_str());
		evbuffer_drain(input, len);
		status = FINISHED;
	}
	status = FINISHED;
	return true;
}

bool Http::loop()
{
	bool flag = false;
	switch(status)
	{
		case REQUEST_LINE:
			flag = parse_request_line();
			break;

		case HEADER:
			flag = parse_header();
			break;

		case MSG_BODY:
			flag = parse_msg_body();
			break;

		case FINISHED:
			flag = excute();
			break;

		case ERROR_STATUS:
			flag = false;
			break;
	}
	return flag;
}
bool Http::excute()
{
	assert(status == FINISHED);		
	DEBUG_LOG("excute");
	if(method != "GET" && method != "POST")
	{
		not_implement(); //501
		return false;
	}
	string file = path;
	if (path=="/")
	{
		file = "./";
	}
	file = "." + path;
	//普通文件返回0，目录返回1
	int isFile = judge_dir_or_file(file);
	if (0 == isFile)
	{
		printf("send file begin\n");
		struct stat st;
		stat(file.c_str(), &st);
		printf("the file size is %d\n", st.st_size);
		send_file(file, st.st_size);

	}
	else if(1==isFile)
	{	
		string str = "HTTP/1.1 200 OK\r\n";
		str += "Content-type: text/html\r\n\r\n";
		bufferevent_write(bev, str.c_str(), strlen(str.c_str()));
		//传输目录
		send_dir(bev,file.c_str(),NULL);
	}
	else
	{
		DEBUG_LOG("not a aviable dir");
	}

	return false;//这次传送完就结束了
}

int judge_dir_or_file(string file)//判断目录还是文件
{
	struct stat st;
	int ret = stat(file.c_str(), &st);
	if (ret == -1)
	{
		return -1;
	}
	if (S_ISREG(st.st_mode))//
	{
		return 0;
	}
	if (S_ISDIR(st.st_mode))
	{
		return 1; 
	}
}

int Http::send_dir(bufferevent * bufev,const char * dirname, void * arg)//拼接html发送目录
{
	printf("dir name is %s\n", dirname);
	char *buf_dir = (char*)malloc(4096);
	struct dirent **ptr;
	//alphasort内部排序方式
	int dir_num = scandir(dirname, &ptr, NULL, alphasort);
	//html head
	sprintf(buf_dir, "<!doctype HTML>\
			<html>\
				<head>\
			<title>Current dir:%s</title>\
			</head>\
		<body>\
		<h1>Current Dir Content:%s</h1>\
		<table>\
		<tr>\
			<td>Name</td><td>Size</td><td>Type</td>\
		</tr>",dirname,dirname);

	bufferevent_write(bufev,buf_dir,strlen(buf_dir));

	memset((char*)buf_dir, 0, sizeof(buf_dir));
	for (int i = 0; i < dir_num; i++)
	{
		char buf_cur_name[1024] = { 0 };
		if (0 == strcmp(dirname, "./"))
		{
			sprintf(buf_cur_name, "%s%s", dirname, ptr[i]->d_name);
		}
		else
		{
			sprintf(buf_cur_name, "%s/%s", dirname, ptr[i]->d_name);
		}
	
		struct stat st;
		memset(&st, 0, sizeof(st));
		stat(buf_cur_name, &st);

		sprintf(buf_dir, "<tr>\
				          <td><a href=\"%s\">%s</a></td>\
				          <td>%ld</td>\
				          <td>%s</td>\
				          </tr>",buf_cur_name,ptr[i]->d_name,st.st_size,
		judge_dir_or_file(buf_cur_name) != 0 ? "dir" : "plain file");
		bufferevent_write(bufev, buf_dir, strlen(buf_dir));

		memset((char*)buf_dir, 0, sizeof(buf_dir));
	}
	printf("111");
	//html end
	sprintf(buf_dir, "</table>\
		               </body>\
		               </html>");
	bufferevent_write(bufev, buf_dir, strlen(buf_dir));
	bufferevent_write(bufev, "\r\n", 2);

	free(buf_dir);
	return 0;
}
//bool Http::excute()/转到网页的
//{
//	assert(status == FINISHED);		
//	DEBUG_LOG("excute");
//
//	if(method != "GET" && method != "POST")
//	{
//		not_implement(); //501
//		return false;
//	}
//
//	const string dir = "./files";	
//
//	size_t pos = path.find('?', 0);
//	if(pos == string::npos)
//	{
//		path =  dir + path;	
//		
//		if(path[path.length() - 1] == '/')
//		{
//			path += "index.html";
//		}
//
//		DEBUG_LOG("%s", path.c_str());
//
//		struct stat buf;
//		if(lstat(path.c_str(), &buf) < 0)	
//		{
//			DEBUG_LOG("%s", strerror(errno));	
//			not_found(); //404 	
//			return false;
//		}
//
//		if(S_ISREG(buf.st_mode))
//		{
//			DEBUG_LOG("file");
//			if((buf.st_mode & S_IXUSR)|| 
//				(buf.st_mode & S_IXGRP) ||
//				(buf.st_mode & S_IXOTH))
//			{
//				if(method == "POST")
//				{
//					exec_cgi(path, msg_body);
//				}
//				else
//				{
//					exec_cgi(path);
//				}	
//			}	
//			else
//			{
//				send_file(path, buf.st_size);
//			}
//		}
//		else if(S_ISDIR(buf.st_mode))
//		{
//			path += "index.html";
//			send_file(path, buf.st_size);
//		}
//	}
//	else //get
//	{
//		if(method != "GET")
//		{
//			bad_request();  //400
//			return false;
//		}
//		string query = path.substr(pos + 1);
//		path.erase(pos);
//		path =  dir + path;	
//		DEBUG_LOG("%s", path.c_str());
//		exec_cgi(path, query);
//	}
//	return false;
//}

bool Http::exec_cgi(const string& path, const string& query)
{
	DEBUG_LOG("%s", path.c_str());

	int fd[2];
	if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0)
	{
		DEBUG_LOG("%s", strerror(errno));
		bad_gateway(); //502
		return  false;	
	}

	pid_t pid = fork();
	if(pid < 0)
	{
		DEBUG_LOG("%s", strerror(errno));
		bad_gateway(); //502
		return  false;	
	}
	else if(pid == 0) //son
	{
		DEBUG_LOG("son process");

		close(fd[1]);
		dup2(fd[0], 0);
		dup2(fd[0], 1);
		
		setenv("REMOTE_METHOD", method.c_str(), 1);
		setenv("QUERY_STRING", query.c_str(), 1);

		string name = path;
		size_t pos = path.rfind("/");
		if(pos != string::npos)
		{
			name = path.substr(pos + 1);
		}
	
		if(execl(path.c_str(), name.c_str(), (char*)0) < 0)
		{
			DEBUG_LOG("%s", strerror(errno));
			return  false;	
		}
	}
	else // parent
	{
		DEBUG_LOG("parent process");

		close(fd[0]);
		int val = fcntl(fd[1], F_SETFL, 0);
		if(val < 0)
		{
			DEBUG_LOG("%s", strerror(errno));
			return  false;	
		}

		val = fcntl(fd[1], F_SETFL, val | O_NONBLOCK);
		if(val < 0)
		{
			DEBUG_LOG("%s", strerror(errno));
			return  false;	
		}

		event_base* base = bufferevent_get_base(bev);
		bev_cgi  = bufferevent_socket_new(base, fd[1], 
			BEV_OPT_CLOSE_ON_FREE /*| BEV_OPT_THREADSAFE*/);

		bufferevent_setcb(bev_cgi, read_cb, write_cb, event_cb, this);
		bufferevent_enable(bev_cgi, EV_READ);

		evbuffer* output = bufferevent_get_output(bev);
		string head =  "HTTP/1.1 200 OK\r\n";
		evbuffer_add(output, head.c_str(), head.length());
	}

	return true;
}

bool Http::send_file(const string& path, size_t size)
{
	DEBUG_LOG("%s, %d", path.c_str(), size);
	int fd = open(path.c_str(), O_RDONLY);
	if(fd < 0)
	{
		DEBUG_LOG("%s", strerror(errno));
		return false;
	}	

	evbuffer* output = bufferevent_get_output(bev);
	string str = "HTTP/1.1 200 OK\r\n";
//	str += "Content-type: text/html\r\n";
	
	size_t pos = path.rfind('.');
	string extension;
	if(pos != string::npos)
	{
		extension = path.substr(pos);
	}
	DEBUG_LOG("%d %s", pos, extension.c_str());
	string type = get_type(extension);
	DEBUG_LOG("%s", type.c_str());
	str += type;
	str += "\r\n";
	evbuffer_add(output, str.c_str(), str.length());

	evbuffer_add_file(output, fd, 0, size);
	set_all_send(true);
	return true;
}


#define ERROR(code, msg) \
	do \
	{ \
		evbuffer* output = bufferevent_get_output(bev); \
		string str = "HTTP/1.1 " #code " " msg "\r\n"; \
		str += "Content-type: text/html\r\n"; \
		str += "\r\n"; \
						\
		str +=  "<html>" \
				"<body>" \
				"<h1 align = center>" #code " " msg "</h1>" \
				"</body>"; \
							\
		evbuffer_add(output, str.c_str(), str.length()); \
		set_all_send(true);	\
	} while(0)


void Http::bad_request()
{
	ERROR(400, "Bad Request");
}

void Http::not_found()
{
	ERROR(404, "Not Found");
}

void Http::not_implement()
{
	ERROR(501, "Not Implemented");
}

void Http::bad_gateway()
{
	ERROR( 502, "Bad Gateway");
}
