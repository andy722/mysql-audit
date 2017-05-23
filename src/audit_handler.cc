/*
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*
 * audit_handler.cc
 *
 *  Created on: Feb 6, 2011
 *      Author: guyl
 */
#include "audit_handler.h"
//for definition of sockaddr_un
#include <sys/un.h>
#include <stdio_ext.h>
#include "static_assert.h"

//utility macro to log also with a date as a prefix
#define log_with_date(f, ...) do{\
    struct tm tm_tmp;\
    time_t result= time(NULL);\
    localtime_r(&result, &tm_tmp);\
    fprintf(f, "%02d%02d%02d %2d:%02d:%02d: ",\
                    tm_tmp.tm_year % 100,\
                    tm_tmp.tm_mon+1,\
                    tm_tmp.tm_mday,\
                    tm_tmp.tm_hour,\
                    tm_tmp.tm_min,\
                    tm_tmp.tm_sec);\
    fprintf(f, __VA_ARGS__);\
}while(0)

//regex flags used in compilation
static const int regex_flags = PCRE_DOTALL | PCRE_UTF8 | PCRE_CASELESS | PCRE_DUPNAMES;


//initialize static stuff
ThdOffsets Audit_formatter::thd_offsets = { 0 };
Audit_handler * Audit_handler::m_audit_handler_list[Audit_handler::MAX_AUDIT_HANDLERS_NUM];
const char * Audit_json_formatter::DEF_MSG_DELIMITER = "\\n";

#if MYSQL_VERSION_ID < 50709
#define C_STRING_WITH_LEN(X) ((char *) (X)), ((size_t) (sizeof(X) - 1))
#endif


const char *  Audit_formatter::retrieve_object_type (TABLE_LIST *pObj)
{
    if (table_is_view(pObj))
	{
		return "VIEW";
	}	
	return "TABLE";
}


void Audit_handler::stop_all()
{
    for (size_t i = 0; i < MAX_AUDIT_HANDLERS_NUM; ++i)
    {
        if (m_audit_handler_list[i] != NULL)
        {
            m_audit_handler_list[i]->set_enable(false);
        }
    }
}

void Audit_handler::log_audit_all(ThdSesData *pThdData)
{
    for (size_t i = 0; i < MAX_AUDIT_HANDLERS_NUM; ++i)
    {
        if (m_audit_handler_list[i] != NULL)
        {
            m_audit_handler_list[i]->log_audit(pThdData);
        }
    }
}
		
void Audit_handler::set_enable(bool val)
{
    lock_exclusive();
    if (m_enabled == val) //we are already enabled simply return
    {
        unlock();
        return;
    }
    m_enabled = val;
    if (m_enabled)
    {
        //call the startup of the handler
        handler_start();
    }
    else
    {
        //call the cleanup of the handler
        handler_stop();
    }
    unlock();
}

void Audit_handler::flush()
{
	lock_exclusive();
    if (!m_enabled) //if not running we don't flush
    {
        unlock();
        return;
    }
    //call the cleanup of the handler
    handler_stop();
    //call the startup of the handler
    handler_start();          
	sql_print_information("%s Log flush complete.", AUDIT_LOG_PREFIX);
    unlock();
}

void Audit_handler::log_audit(ThdSesData *pThdData)
{
    lock_shared();
    if (!m_enabled)
    {
        unlock();
        return;
    }	
    //sanity check that offsets match
	//we can also consider using secutiry context function to do some sanity checks
	//  char buffer[2048];
    //  thd_security_context(thd, buffer, 2048, 2000);
    //  fprintf(log_file, "info from security context: %s\n", buffer);
    unsigned long inst_thread_id = Audit_formatter::thd_inst_thread_id(pThdData->getTHD());
    unsigned long plug_thread_id = thd_get_thread_id(pThdData->getTHD());
    if (inst_thread_id != plug_thread_id)
    {
        if (m_print_offset_err)
        {
            m_print_offset_err = false;
            sql_print_error(
                    "%s Thread id from thd_get_thread_id doesn't match calculated value from offset %lu <> %lu. Aborting!",
                    AUDIT_LOG_PREFIX, inst_thread_id, plug_thread_id);
        }		
    }
    else
    {//offsets are good
        m_print_offset_err = true; //mark to print offset err to log incase we encounter in the future
		pthread_mutex_lock(&LOCK_io);
		//check if failed
		bool do_log = true;
		if (m_failed)
		{		
			do_log = false;
			bool retry = m_retry_interval > 0 && 
				difftime(time(NULL), m_last_retry_sec_ts) > m_retry_interval;		
			if(retry)
			{
				do_log = handler_start_nolock();
			}					
		}
		if(do_log)
		{
			if(!handler_log_audit(pThdData))
			{
				set_failed();
				handler_stop_internal();
			}
		}
		pthread_mutex_unlock(&LOCK_io);
    }
    unlock();
}

void Audit_file_handler::close()
{
	if (m_log_file)
	{
		my_fclose(m_log_file, MYF(0));
	}
	m_log_file = NULL;
}

ssize_t Audit_file_handler::write(const char * data, size_t size)
{
    ssize_t res = my_fwrite(m_log_file, (uchar *) data, size, MYF(0));
	if(res < 0) // log the error
	{
		sql_print_error("%s failed writing to file: %s. Err: %s", 
			AUDIT_LOG_PREFIX, m_io_dest, strerror(errno));
	}
	return res;
}

int Audit_file_handler::open(const char * io_dest, bool log_errors)
{
	char format_name[FN_REFLEN];
    fn_format(format_name, io_dest, "", "", MY_UNPACK_FILENAME);
    m_log_file = my_fopen(format_name,  O_WRONLY | O_APPEND| O_CREAT, MYF(0));
	if (!m_log_file)
    {
		if(log_errors)
		{
			sql_print_error(
					"%s unable to open file %s: %s. audit file handler disabled!!",
					AUDIT_LOG_PREFIX, m_io_dest, strerror(errno));
		}
		return -1;
	}
    ssize_t bufsize = BUFSIZ;
    int res =0;
    //0 -> use default, 1 or negative -> disabled
    if(m_bufsize > 1) 
    {
        bufsize = m_bufsize;
    }
    if(1 == m_bufsize || m_bufsize < 0)
    {
        //disabled
        res = setvbuf(m_log_file, NULL,  _IONBF, 0);
    }
    else
    {   
        res = setvbuf(m_log_file, NULL, _IOFBF, bufsize);        
        
    }    
    if(res)
    {
        sql_print_error(
					"%s unable to set bufzie [%zd (%ld)] for file %s: %s.",
					AUDIT_LOG_PREFIX, bufsize, m_bufsize, m_io_dest, strerror(errno));
    }
    sql_print_information("%s bufsize for file [%s]: %zd. Value of json_file_bufsize: %ld.", AUDIT_LOG_PREFIX, m_io_dest, 
        __fbufsize(m_log_file), m_bufsize);
	return 0;
}

//no locks. called by handler_start and when it is time to retry
bool Audit_io_handler::handler_start_internal()
{
	if(!m_io_dest || strlen(m_io_dest) == 0)
	{
		if(m_log_io_errors)
		{
			sql_print_error(
					"%s %s: io destination not set. Not connecting.",
					AUDIT_LOG_PREFIX,  m_io_type);
		}
        return false;
	}
	if (open(m_io_dest, m_log_io_errors) != 0)
    {
		//open failed        
		return false;
    }
    ssize_t res = m_formatter->start_msg_format(this);
	/*
	 sanity check of writing to the log. If we fail. We will print an erorr and disable this handler.
	 */
	if (res < 0)
	{
		if(m_log_io_errors)
		{
			sql_print_error(
					"%s unable to write header msg to %s: %s.",
					AUDIT_LOG_PREFIX, m_io_dest, strerror(errno));
		}
		close();		
		return false;
	}
	sql_print_information("%s success opening %s: %s.", AUDIT_LOG_PREFIX, m_io_type, m_io_dest);
	return true;    
}

void Audit_io_handler::handler_stop_internal()
{
	if(!m_failed)
	{
		m_formatter->stop_msg_format(this);
	}
    close();
}

bool Audit_handler::handler_start_nolock()
{
	bool res = handler_start_internal();
	if(res)
	{
		m_failed = false;
	}
	else
	{
		set_failed();
	}
	return res;
}

void Audit_handler::handler_start()
{
    pthread_mutex_lock(&LOCK_io);    
	m_log_io_errors = true;
    handler_start_nolock();
    pthread_mutex_unlock(&LOCK_io);
}
void Audit_handler::handler_stop()
{
    pthread_mutex_lock(&LOCK_io);
    handler_stop_internal();
    pthread_mutex_unlock(&LOCK_io);
}

bool Audit_file_handler::handler_log_audit(ThdSesData *pThdData)
{
    bool res = (m_formatter->event_format(pThdData, this) >= 0);
    if (res && m_sync_period && ++m_sync_counter >= m_sync_period)
    {
        m_sync_counter = 0;
        //Note fflush() only flushes the user space buffers provided by the C library.
        //To ensure that the data is physically stored on disk the kernel buffers must be flushed too,
        //e.g. with sync(2) or fsync(2).
		res = (fflush(m_log_file) == 0);
        if(res)
		{
			int fd = fileno(m_log_file);
			res = (my_sync(fd, MYF(MY_WME)) == 0);
		}
    }
	return res;
}

/////////////////// Audit_socket_handler //////////////////////////////////

void Audit_socket_handler::close()
{
	if (m_vio)
	{
		//no need for vio_close as is called by delete (additionally close changed its name to vio_shutdown in 5.6.11)
		vio_delete((Vio*)m_vio);
	}
	m_vio = NULL;
}

ssize_t Audit_socket_handler::write(const char * data, size_t size)
{
    ssize_t res = vio_write((Vio*)m_vio, (const uchar *) data, size);
	if(res < 0) // log the error
	{
		sql_print_error("%s failed writing to socket: %s. Err: %s", 
			AUDIT_LOG_PREFIX, m_io_dest, strerror(vio_errno((Vio*)m_vio)));
	}
	return res;
}

int Audit_socket_handler::open(const char * io_dest, bool log_errors)
{	
	//open the socket
    int sock = socket(AF_UNIX,SOCK_STREAM,0);
    if (sock < 0)
    {
		if(log_errors)
		{
			sql_print_error(
					"%s unable to create unix socket: %s.",
					AUDIT_LOG_PREFIX, strerror(errno));
		}
        return -1;
    }

    //connect the socket
    m_vio= vio_new(sock, VIO_TYPE_SOCKET, VIO_LOCALHOST);
    struct sockaddr_un UNIXaddr;
    UNIXaddr.sun_family = AF_UNIX;
    strmake(UNIXaddr.sun_path, io_dest, sizeof(UNIXaddr.sun_path)-1);
#if MYSQL_VERSION_ID < 50600
    if (my_connect(sock,(struct sockaddr *) &UNIXaddr, sizeof(UNIXaddr),
                   m_connect_timeout))
#else
    //in 5.6 timeout is in ms
    if (vio_socket_connect((Vio*)m_vio,(struct sockaddr *) &UNIXaddr, sizeof(UNIXaddr),
                           m_connect_timeout * 1000))
#endif
    {
		if(log_errors)
		{
			sql_print_error(
                "%s unable to connect to socket: %s. err: %s.",
                AUDIT_LOG_PREFIX, m_io_dest, strerror(errno));
		}
		close();
		return -2;
	}
	return 0;
}

bool Audit_socket_handler::handler_log_audit(ThdSesData *pThdData)
{    
    return (m_formatter->event_format(pThdData, this) >= 0);    
}

//////////////////////// Audit Socket handler end ///////////////////////////////////////////




static  yajl_gen_status yajl_add_string(yajl_gen hand, const char * str)
{
    return yajl_gen_string(hand, (const unsigned char*)str, strlen(str));
}

static  void yajl_add_string_val(yajl_gen hand, const char * name, const char* val)
{
	if(0 == val)
	{
		return; //we don't add NULL values to json
	}
    yajl_add_string(hand, name);
    yajl_add_string(hand, val);
}

static  void yajl_add_string_val(yajl_gen hand, const char * name, const char* val, size_t val_len)
{
    yajl_add_string(hand, name);
    yajl_gen_string(hand, (const unsigned char*)val, val_len);
}

static  void yajl_add_uint64(yajl_gen gen, const char * name, uint64 num)
{
    const size_t max_int64_str_len = 21;
    char buf[max_int64_str_len];
    snprintf(buf, max_int64_str_len, "%llu", num);
    yajl_add_string_val(gen, name, buf);
}
static  void yajl_add_obj( yajl_gen gen,  const char *db,const char* ptype,const char * name =NULL)
{
    if(db)
    {
        yajl_add_string_val(gen, "db", db);
    }
    if (name)
    {
        yajl_add_string_val(gen, "name", name);
    }
    yajl_add_string_val(gen, "obj_type",ptype);
}

static  const char * retrieve_user (THD * thd)
{
    
	const char * user = Audit_formatter::thd_inst_main_security_ctx_user(thd);
	if(user != NULL && *user != 0x0) //non empty
	{
		return user;
	}	
	user = Audit_formatter::thd_inst_main_security_ctx_priv_user(thd); //try using priv user
	if(user != NULL && *user != 0x0) //non empty
	{
		return user;
	}
	return ""; //always use at least the empty string    
}


//will return a pointer to the query and set len with the length of the query
//starting with MySQL version 5.1.41 thd_query_string is added
//And at 5.7 it changed
#if ! defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50706

extern "C" LEX_CSTRING thd_query_unsafe(MYSQL_THD thd);

static const char * thd_query_str(THD * thd, size_t * len)
{
    const LEX_CSTRING str = thd_query_unsafe(thd);
    if(str.length > 0)
    {
        *len = str.length;
        return str.str;
    }
    *len = 0;
    return NULL;
}

#elif defined(MARIADB_BASE_VERSION) || MYSQL_VERSION_ID > 50140

extern "C" {
    MYSQL_LEX_STRING *thd_query_string(MYSQL_THD thd);
}
static const char * thd_query_str(THD * thd, size_t * len)
{
    MYSQL_LEX_STRING * str = thd_query_string(thd);
    if(str)
    {
        *len = str->length;
        return str->str;
    }
    *len = 0;
    return NULL;
}
#else
//we are being compiled against mysql version 5.1.40 or lower (our default compilation env)
//we still want to support thd_query_string if we are run on a version higher than 5.1.40, so we try to lookup the symbol
static LEX_STRING * (*thd_query_string_func)(THD *thd) = (LEX_STRING*(*)(THD*))dlsym(RTLD_DEFAULT, "thd_query_string");
static bool print_thd_query_string_func = true; //debug info print only once
static const char * thd_query_str(THD * thd, size_t * len)
{
    if(print_thd_query_string_func)
    {
        sql_print_information("%s thd_query_string_func: 0x%lx", AUDIT_LOG_PREFIX, (unsigned long)thd_query_string_func);
        print_thd_query_string_func = false;
    }
    if(thd_query_string_func)
    {
        MYSQL_LEX_STRING * str = thd_query_string_func(thd);
        if(str)
        {
            *len = str->length;
            return str->str;
        }
        *len = 0;
        return NULL;
    }
    *len = thd->query_length;
    return thd->query;
}
#endif

ssize_t Audit_json_formatter::start_msg_format(IWriter * writer)
{
    if(!m_write_start_msg) //disabled
    {
        return 0;
    }
	//initialize yajl
    yajl_gen gen = yajl_gen_alloc(&config, NULL);
    yajl_gen_map_open(gen);
    yajl_add_string_val(gen, "msg-type", "header");
	uint64 ts = my_getsystime() / (10000);
	yajl_add_uint64(gen, "date", ts);
	yajl_add_string_val(gen, "audit-version", MYSQL_AUDIT_PLUGIN_VERSION"-"MYSQL_AUDIT_PLUGIN_REVISION);
	yajl_add_string_val(gen, "audit-protocol-version", AUDIT_PROTOCOL_VERSION);	
	yajl_add_string_val(gen, "hostname", glob_hostname);
	yajl_add_string_val(gen, "mysql-version", server_version);	
	yajl_add_string_val(gen, "mysql-program", my_progname);	
	yajl_add_string_val(gen, "mysql-socket", mysqld_unix_port);	
	yajl_add_uint64(gen, "mysql-port", mysqld_port);	
	ssize_t res = -2;
	yajl_gen_status stat = yajl_gen_map_close(gen); //close the object
    if(stat == yajl_gen_status_ok) //all is good write the buffer out
    {
        const unsigned char * text = NULL;
        unsigned int len = 0;
        yajl_gen_get_buf(gen, &text, &len);
        //print the json
        res = writer->write((const char *)text, len);
        if(res >= 0)
        {
            //TODO: use the msg_delimiter
            res = writer->write("\n", 1);
        }
        //my_fwrite(log_file, (uchar *) b.data, json_size(&b), MYF(0));
    }
    yajl_gen_free(gen); //free the generator
    return res;
	
}

// This routine replaces clear text with the string in `replace', leaving the rest of the string intact.
//
// thd			- MySQL thread, used for allocating memory
// str			- pointer to start of original string
// str_len		- length thereof
// cleartext_start	- start of cleartext to replace
// cleartext_len	- length of cleartext
// replace		- \0 terminated string with replacement text
static const char *replace_in_string(THD *thd,
					const char *str, size_t str_len,
					size_t cleartext_start, size_t cleartext_len,
					const char *replace)
{
	size_t to_alloc = str_len + strlen(replace) + 1;
	char *new_str = (char *) thd_alloc(thd, to_alloc);
	memset(new_str, '\0', to_alloc);

	// point to text after clear text
	const char *trailing = str + cleartext_start + cleartext_len;
	// how much text after clear text to copy in
	size_t final_to_move = ((str + str_len) - trailing);

	char *pos = new_str;
	memcpy(pos, str, cleartext_start);	// copy front of string
	pos += cleartext_start;

	memcpy(pos, replace, strlen(replace));	// copy replacement text
	pos += strlen(replace);

	memcpy(pos, trailing, final_to_move);	// copy trailing part of string

	return new_str;
}

ssize_t Audit_json_formatter::event_format(ThdSesData* pThdData, IWriter * writer)
{
	THD * thd = pThdData->getTHD();
    unsigned long thdid = thd_get_thread_id(thd);
    query_id_t qid = thd_inst_query_id(thd);	

    //initialize yajl
    yajl_gen gen = yajl_gen_alloc(&config, NULL);
    yajl_gen_map_open(gen);
    yajl_add_string_val(gen, "msg-type", "activity");
    //TODO: get the start date from THD (but it is not in millis. Need to think about how we handle this)
    //for now simply use the current time.
    //my_getsystime() time since epoc in 100 nanosec units. Need to devide by 1000*(1000/100) to reach millis
    uint64 ts = my_getsystime() / (10000);
    yajl_add_uint64(gen, "date", ts);
    yajl_add_uint64(gen, "thread-id", thdid);
    yajl_add_uint64(gen, "query-id", qid);
	yajl_add_string_val(gen, "user", pThdData->getUserName());
	yajl_add_string_val(gen, "priv_user", Audit_formatter::thd_inst_main_security_ctx_priv_user(thd));
	yajl_add_string_val(gen, "host", Audit_formatter::thd_inst_main_security_ctx_host(thd));
    yajl_add_string_val(gen, "ip", Audit_formatter::thd_inst_main_security_ctx_ip(thd));    
    const char *cmd = pThdData->getCmdName();
    yajl_add_string_val(gen, "cmd", cmd);
    //get objects
    if(pThdData->startGetObjects())
    {
        yajl_add_string(gen, "objects");
        yajl_gen_array_open(gen);
        const char * db_name = NULL;
        const char * obj_name = NULL;
        const char * obj_type = NULL;
        while(pThdData->getNextObject(&db_name, &obj_name, &obj_type))
        {
            yajl_gen_map_open(gen);
            yajl_add_obj (gen, db_name, obj_type, obj_name );
            yajl_gen_map_close(gen);
        }
        yajl_gen_array_close(gen);
    }

    size_t qlen = 0;
    const char * query = thd_query_str(pThdData->getTHD(), &qlen);
    if (query && qlen > 0)
    {
#if MYSQL_VERSION_ID < 50600
        CHARSET_INFO *col_connection;
#else
        const CHARSET_INFO *col_connection;
#endif
		col_connection = Item::default_charset();

		// See comment below as to why we don't use String class directly, or call
		// pThdData->getTHD()->convert_string (&sQuery,col_connection,&my_charset_utf8_general_ci);
		const char *query_text = query;
		size_t query_len = qlen;

		if (strcmp(col_connection->csname, "utf8") != 0) {
			// max UTF-8 bytes per char is 4.
			size_t to_amount = (qlen * 4) + 1;
			char* to = (char *) thd_alloc(thd, to_amount);

			uint errors = 0;

			size_t len = copy_and_convert(to, to_amount,
				&my_charset_utf8_general_ci,
				query, qlen,
				col_connection, & errors);

			to[len] = '\0';

			query = to;
			qlen = len;
		}

		if(m_perform_password_masking && m_password_mask_regex_compiled && m_password_mask_regex_preg && m_perform_password_masking(cmd))
		{
			//do password masking
			int matches[90] = {0};
			if(pcre_exec(m_password_mask_regex_preg, NULL, query_text, query_len, 0, 0, matches,  array_elements(matches)) >= 0)
			{
				//search for the first substring that matches with the name psw
				char *first = NULL, *last = NULL;
				int entrysize = pcre_get_stringtable_entries(m_password_mask_regex_preg, "psw", &first, &last);
				if(entrysize > 0)
				{
					for (unsigned char * entry = (unsigned char *)first; entry <= (unsigned char *)last; entry += entrysize)
					{
						//first 2 bytes give us the number
						int n = (((int)(entry)[0]) << 8) | (entry)[1];
						if (n > 0 && n < (int)array_elements(matches) && matches[n*2] >= 0)
						{
							// We have a match.

							// Starting with MySQL 5.7, we cannot use the String::replace() function.
							// Doing so causes a crash in the string's destructor. It appears that the
							// interfaces in MySQL have changed fairly drastically. So we just do the
							// replacement ourselves.
							const char *pass_replace = "***";
							const char *updated = replace_in_string(thd, query_text, query_len, matches[n*2], matches[(n*2) + 1] - matches[n*2], pass_replace);
							query_text = updated;
							query_len = strlen(query_text);
							break;
						}
					}
				}								
			}
		}
		yajl_add_string_val(gen, "query", query_text, query_len);
    }
    else 
    {
        if (cmd!=NULL && strlen (cmd)!=0)
        {
            yajl_add_string_val(gen, "query",cmd, strlen (cmd));
        }
        else 
        {
            yajl_add_string_val(gen, "query","n/a", strlen ("n/a"    ));
        }
    }
    ssize_t res = -2;
    yajl_gen_status stat = yajl_gen_map_close(gen); //close the object
    if(stat == yajl_gen_status_ok) //all is good write the buffer out
    {
        const unsigned char * text = NULL;
        unsigned int len = 0;
        yajl_gen_get_buf(gen, &text, &len);
        //print the json
        res = writer->write((const char *)text, len);
        if(res >= 0)
        {
            //TODO: use the msg_delimiter
            res = writer->write("\n", 1);
        }
        //my_fwrite(log_file, (uchar *) b.data, json_size(&b), MYF(0));
    }
    yajl_gen_free(gen); //free the generator
    return res;
}



ThdSesData::ThdSesData (THD *pTHD) :
        m_pThd (pTHD), m_CmdName(NULL), m_UserName(NULL),
        m_objIterType(OBJ_NONE), m_tables(NULL), m_firstTable(true),
        m_tableInf(NULL), m_index(0), m_isSqlCmd(false)
{
    m_CmdName = retrieve_command (m_pThd, m_isSqlCmd);
    m_UserName = retrieve_user (m_pThd);
}

bool ThdSesData::startGetObjects()
{
    //reset vars as this may be called multiple times
    m_objIterType = OBJ_NONE;
    m_tables = NULL;
    m_firstTable = true;
    m_index = 0;
    m_tableInf =  Audit_formatter::getQueryCacheTableList1(getTHD());
    int command = Audit_formatter::thd_inst_command(getTHD());
    LEX * pLex = Audit_formatter::thd_lex(getTHD());
    //query cache case
    if(pLex && command == COM_QUERY && m_tableInf && m_tableInf->num_of_elem > 0)
    {
        m_objIterType = OBJ_QUERY_CACHE;
        return true;
    }
    const char *cmd = getCmdName();
    //commands which have single database object
    if (strcmp (cmd,"Init DB") ==0
        || strcmp (cmd, "SHOW TABLES")== 0
        || strcmp (cmd,  "SHOW TABLE")==0)
    {
        if(Audit_formatter::thd_db(getTHD()))
        {
            m_objIterType = OBJ_DB;
            return true;
        }
        return false;
    }
    //only return query tabls if command is COM_QUERY
    //TODO: check if other commands can also generate query tables such as "show fields"
    if (pLex && command == COM_QUERY && pLex->query_tables)
    {
        m_tables = pLex->query_tables;
        m_objIterType = OBJ_TABLE_LIST;
        return true;
    }
    //no objects
    return false;
}

bool ThdSesData::getNextObject(const char ** db_name, const char ** obj_name, const char ** obj_type)
{
    switch(m_objIterType)
    {
        case OBJ_DB:
        {
            if(m_firstTable)
            {
                *db_name = Audit_formatter::thd_db(getTHD());
                *obj_name = NULL;
                if(obj_type)
                {
                    *obj_type = "DATABASE";
                }
                m_firstTable = false;
                return true;
            }
            return false;
        }
        case OBJ_QUERY_CACHE:
        {
            if(m_index < m_tableInf->num_of_elem &&
                    m_index< MAX_NUM_QUERY_TABLE_ELEM)
            {
                *db_name = m_tableInf->db[m_index];
                *obj_name = m_tableInf->table_name[m_index];
                if(obj_type)
                {
                    *obj_type = m_tableInf->object_type[m_index];
                }
                m_index++;
                return true;
            }
            return false;
        }
        case OBJ_TABLE_LIST:
        {
            if(m_tables)
            {
                *db_name = Audit_formatter::table_get_db_name(m_tables);
                *obj_name = Audit_formatter::table_get_name(m_tables);
                if(obj_type)
                {
                    //object is a view if it view command (alter_view, drop_view ..)
                    //and first object or view field is populated
                    if((m_firstTable && strstr(getCmdName(), "_view") != NULL) ||
                            Audit_formatter::table_is_view(m_tables))
                    {
                        *obj_type = "VIEW";
                        m_firstTable = false;
                    }
                    else
                    {
                        *obj_type = "TABLE";
                    }
                }
                m_tables = m_tables->next_global;
                return true;
            }
            return false;
        }
        default :
            return false;
    }
}

pcre * Audit_json_formatter::regex_compile(const char * str)
{
	const char *error;
	int erroffset;
	pcre * re = pcre_compile(str, regex_flags, &error, &erroffset, NULL);
	if (!re)
	{
		sql_print_error("%s unable to compile regex [%s]. offset: %d message: [%s].",
                AUDIT_LOG_PREFIX, str, erroffset, error);		
	}
	return re;
}

int Audit_json_formatter::compile_password_masking_regex(const char * str)
{
	//first free existing
	if(m_password_mask_regex_compiled)
	{
		m_password_mask_regex_compiled = false;
		//small sleep to let threads oomplete regexc
		my_sleep(10 * 1000);
		pcre_free(m_password_mask_regex_preg);		
	}
	int error = 1; //default is error (case of empty string)
	if(NULL != str && str[0] != '\0')
	{
		m_password_mask_regex_preg =  regex_compile(str);
		if(m_password_mask_regex_preg)
		{
			m_password_mask_regex_compiled = true;
			error = 0;
		}		
	}
	return error;
}
