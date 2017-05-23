/*
 * audit_handler.h
 *
 *  Created on: Feb 6, 2011
 *      Author: guyl
 */

#ifndef AUDIT_HANDLER_H_
#define AUDIT_HANDLER_H_

#include "mysql_inc.h"
#include <yajl/yajl_gen.h>

#ifndef PCRE_STATIC
#define PCRE_STATIC
#endif

#include <pcre.h>

#define AUDIT_LOG_PREFIX "Audit Plugin:"
#define AUDIT_PROTOCOL_VERSION "1.0"

#if !defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50706
// For locking we use the native lock routines provided by MySQL.
// The data types and functions for native locking changed at 5.7.x.
// Try to hide this with macros.
#define rw_lock_t	native_rw_lock_t
#define rw_rdlock	native_rw_rdlock
#define rw_wrlock	native_rw_wrlock
#define rw_unlock	native_rw_unlock
#define rwlock_destroy	native_rw_destroy
#define my_rwlock_init(lock, unused)	native_rw_init(lock)
#endif

class THD;

#define MAX_NUM_QUERY_TABLE_ELEM 100
typedef struct _QueryTableInf {
	int num_of_elem;
	char *db[MAX_NUM_QUERY_TABLE_ELEM];
	char *table_name[MAX_NUM_QUERY_TABLE_ELEM];
	const char *object_type [MAX_NUM_QUERY_TABLE_ELEM];
} QueryTableInf; 

#define MAX_NUM_QUEUE_ELEM 1024
typedef struct _THDPRINTED {
    size_t cur_index;
    char is_thd_printed_queue [MAX_NUM_QUEUE_ELEM];
} THDPRINTED;

#define MAX_COMMAND_CHAR_NUMBERS 40
const char * retrieve_command (THD * thd, bool & is_sql_cmd);
typedef size_t OFFSET;

#define MAX_COM_STATUS_VARS_RECORDS 512

//mysql max identifier is 64 so 2*64 + . and null
#define MAX_OBJECT_CHAR_NUMBERS 131
#define MAX_USER_CHAR_NUMBERS 20
#define MAX_NUM_OBJECT_ELEM 256
#define MAX_NUM_USER_ELEM 256

/**
 * The struct used to hold offsets. We should have one per version.
 */
typedef struct ThdOffsets
{
    const char * version;
	const char * md5digest;
    OFFSET query_id;
    OFFSET thread_id;
    OFFSET main_security_ctx;
    OFFSET command;
	OFFSET lex;
	OFFSET lex_comment;
	OFFSET sec_ctx_user;
	OFFSET sec_ctx_host;
	OFFSET sec_ctx_ip;
	OFFSET sec_ctx_priv_user;
    OFFSET db;
    OFFSET killed;
} ThdOffsets;

/*
 * The offsets array
 */
extern const ThdOffsets thd_offsets_arr[];
extern const size_t thd_offsets_arr_size;

/*
 * On  success,  the  number of bytes written are returned (zero indicates nothing was written).  On error, -1 is returned,
 */
typedef ssize_t (*audit_write_func)(const char *, size_t);


/**
 * Interface for an io writer
 */
class IWriter
{
public:
    virtual ~IWriter() {}
	//return negative on fail
    virtual ssize_t write(const char * data, size_t size) = 0;
    inline ssize_t write_str(const char * str)
    {
        return write(str, strlen(str));
    }
	//return 0 on success
	virtual int open(const char * io_dest, bool log_errors) = 0;
	virtual void close() = 0;	
};

class ThdSesData {
public:

    //enum indicating from where the object list came from
    enum ObjectIterType {OBJ_NONE, OBJ_DB, OBJ_QUERY_CACHE, OBJ_TABLE_LIST};
    ThdSesData(THD *pTHD);
    THD* getTHD () { return m_pThd;}
    const char * getCmdName () { return m_CmdName; }
    const char * getUserName () { return m_UserName; }
    /**
     * Start fetching objects. Return true if there are objects available.
     */
    bool startGetObjects();
    /**
     * Get next object. Return true if populated. False if there isn't an object available.
     * Will point the passed pointers to point to db, name and type.
     * obj_type is optional and may be null.
     */
    bool getNextObject(const char ** db_name, const char ** obj_name, const char ** obj_type);
private:
    THD *m_pThd;
    const char *m_CmdName;
    const char *m_UserName;
    bool m_isSqlCmd;
    enum ObjectIterType m_objIterType;
    //pointer for iterating tables
    TABLE_LIST * m_tables;
    //indicator if we are at the first table
    bool m_firstTable;
    //used for query cache iter
    QueryTableInf * m_tableInf;
    int m_index;
protected:
    ThdSesData (const ThdSesData& );
    ThdSesData &operator =(const ThdSesData& );
};
 
/**
 * Base for audit formatter
 */
class Audit_formatter
{
public:

    virtual ~Audit_formatter() {}

    /**
     * static offsets to use for fetching THD data. Set by the audit plugin during startup.
     */
    static ThdOffsets thd_offsets;

    /**
     * Format an audit event from the passed THD. Will write out its output using the audit_write_func.
     *
     * @return -1 on a failure
     */
    virtual ssize_t event_format(ThdSesData *pThdData, IWriter * writer) =0;
    /**
     * format a message when handler is started
     * @return -1 on a failure
     */
    virtual ssize_t start_msg_format(IWriter * writer) { return 0; }
    /**
     * format a message when handler is stopped
     * @return -1 on a failure
     */
    virtual ssize_t stop_msg_format(IWriter * writer) { return 0; }

	static const char * retrieve_object_type (TABLE_LIST *pObj);
	static QueryTableInf* getQueryCacheTableList1 (THD *thd);
    //utility functions for fetching thd stuff
    static inline my_thread_id thd_inst_thread_id(THD * thd)
    {
        return *(my_thread_id *) (((unsigned char *) thd)
                + Audit_formatter::thd_offsets.thread_id);
    }
    static inline query_id_t thd_inst_query_id(THD * thd)
    {
        return *(query_id_t *) (((unsigned char *) thd)
                + Audit_formatter::thd_offsets.query_id);
    }
    static inline Security_context * thd_inst_main_security_ctx(THD * thd)
    {
        return (Security_context *) (((unsigned char *) thd)
                + Audit_formatter::thd_offsets.main_security_ctx);
    }
	
    static inline const char * thd_db(THD * thd)
    {		
		if(!Audit_formatter::thd_offsets.db) //no offsets use compiled in header
		{
#if defined(MARIADB_BASE_VERSION) || MYSQL_VERSION_ID < 50706
 			return thd->db;
#else
			return thd->db().str;
#endif
		}
        return *(const char **) (((unsigned char *) thd)
                + Audit_formatter::thd_offsets.db);        
    }
    
    static inline int thd_killed(THD * thd)
    {		
        if(!Audit_formatter::thd_offsets.killed) //no offsets use thd_killed function
		{
			return ::thd_killed(thd);
		}
        return *(int *) (((unsigned char *) thd)
                + Audit_formatter::thd_offsets.killed);        
    }
    
	static inline const char * thd_inst_main_security_ctx_user(THD * thd)
    {
		Security_context * sctx = thd_inst_main_security_ctx(thd);
		if(!Audit_formatter::thd_offsets.sec_ctx_user) //no offsets use compiled in header
		{
#if defined(MARIADB_BASE_VERSION) || MYSQL_VERSION_ID < 50706
 			return sctx->user;
#else
			return sctx->user().str;
#endif
		}		
        return *(const char **) (((unsigned char *) sctx)
                + Audit_formatter::thd_offsets.sec_ctx_user);
    }
	
	static inline const char * thd_inst_main_security_ctx_host(THD * thd)
    {
		Security_context * sctx = thd_inst_main_security_ctx(thd);
		if(!Audit_formatter::thd_offsets.sec_ctx_ip) //check ip to understand if set as host is first and may actually be set to 0
		{
		//interface changed in 5.5.34 and 5.6.14 and up host changed to get_host()
		//see: http://bazaar.launchpad.net/~mysql/mysql-server/5.5/revision/4407.1.1/sql/sql_class.h
#if defined(MARIADB_BASE_VERSION) 
		return sctx->host;
#else
		// MySQL
#if  MYSQL_VERSION_ID < 50534 || (MYSQL_VERSION_ID >= 50600 && MYSQL_VERSION_ID < 50614)
		return sctx->host;
#elif (MYSQL_VERSION_ID >= 50534 && MYSQL_VERSION_ID < 50600) \
	|| (MYSQL_VERSION_ID >= 50614 &&  MYSQL_VERSION_ID < 50706)
 		return sctx->get_host()->ptr();
#else
		// interface changed again in 5.7
		return sctx->host().str;
#endif
#endif // ! defined(MARIADB_BASE_VERSION)
		}
        return *(const char **) (((unsigned char *) sctx)
                + Audit_formatter::thd_offsets.sec_ctx_host);
    }
	
	static inline const char * thd_inst_main_security_ctx_ip(THD * thd)
    {
		Security_context * sctx = thd_inst_main_security_ctx(thd);
		if(!Audit_formatter::thd_offsets.sec_ctx_ip) //no offsets use compiled in header
		{
//interface changed in 5.5.34 and 5.6.14 and up host changed to get_ip()
#if defined(MARIADB_BASE_VERSION) 
		return sctx->ip;
#else
		// MySQL
#if  MYSQL_VERSION_ID < 50534 || (MYSQL_VERSION_ID >= 50600 && MYSQL_VERSION_ID < 50614)
		return sctx->ip;
#elif (MYSQL_VERSION_ID >= 50534 && MYSQL_VERSION_ID < 50600) \
	|| (MYSQL_VERSION_ID >= 50614 &&  MYSQL_VERSION_ID < 50706)
		return sctx->get_ip()->ptr();
#else
		// interface changed again in 5.7
		return sctx->ip().str;
#endif
#endif // ! defined(MARIADB_BASE_VERSION)
		}		
        return *(const char **) (((unsigned char *) sctx)
                + Audit_formatter::thd_offsets.sec_ctx_ip);
    }
	
	static inline const char * thd_inst_main_security_ctx_priv_user(THD * thd)
    {
		Security_context * sctx = thd_inst_main_security_ctx(thd);
		if(!Audit_formatter::thd_offsets.sec_ctx_priv_user) //no offsets use compiled in header
		{
#if defined(MARIADB_BASE_VERSION) || MYSQL_VERSION_ID < 50706
 			return sctx->priv_user;
#else
			return sctx->priv_user().str;
#endif
		}		
#if MYSQL_VERSION_ID < 50505 
		//in 5.1.x priv_user is a pointer
		return *(const char **) (((unsigned char *) sctx)
                + Audit_formatter::thd_offsets.sec_ctx_priv_user);
#else
		//in 5.5 and up priv_user is an array (char   priv_user[USERNAME_LENGTH])
        return (const char *) (((unsigned char *) sctx)
                + Audit_formatter::thd_offsets.sec_ctx_priv_user);
#endif
    }
	
	static inline int thd_inst_command(THD * thd)
    {
        return *(int *) (((unsigned char *) thd) + Audit_formatter::thd_offsets.command);
    }

	static inline LEX* thd_lex(THD * thd)
    {
		return *(LEX**) (((unsigned char *) thd) + Audit_formatter::thd_offsets.lex);
    }
	
	//we don't use get_db_name() as when we call it view may be not null and it may return an invalid value for view_db 
	static inline const char * table_get_db_name(TABLE_LIST * table)
	{
		return table->db;
	}
	
	static inline const char * table_get_name(TABLE_LIST * table)
	{
		return table->table_name;
	}
	
	static inline bool table_is_view(TABLE_LIST * table)	
	{
		return table->view_tables != 0;
	}

};


/**
 * Format the audit even in json format
 */
class Audit_json_formatter: public Audit_formatter
{
public:

    static const char * DEF_MSG_DELIMITER;

    Audit_json_formatter(): m_msg_delimiter(NULL), m_write_start_msg(true), m_password_mask_regex_preg(NULL),
		m_password_mask_regex_compiled(false), m_perform_password_masking(NULL)
    {
        config.beautify = 0;
        config.indentString = NULL;
    }
    virtual ~Audit_json_formatter() 
	{
		if(m_password_mask_regex_preg)
		{
			m_password_mask_regex_compiled = false;
			pcre_free(m_password_mask_regex_preg);
			m_password_mask_regex_preg = NULL;			
		}
	}
	
    virtual ssize_t event_format(ThdSesData *pThdData, IWriter * writer);
	virtual ssize_t start_msg_format(IWriter * writer);		
	
	/**
	 * Utility method used to compile a regex program. Will compile and log errors if necessary.
	 * Return null if fails
	 */
	static pcre * regex_compile(const char * str);
	
	/**
	 * Compile password masking regex
	 * Return 0 on success
	 */
	int compile_password_masking_regex(const char * str);

	/**
	 * Boolean indicating if to log start msg.
	 * Public so sysvar can update.
	 */
	my_bool m_write_start_msg;		
	
	
	/**
	 * Callback function to determine if password masking should be performed
	 */
	my_bool (* m_perform_password_masking)(const char *cmd);

    /**
     * Message delimiter. Should point to a valid json string (supporting the json escapping format).
     * Will only be checked at the start. Public so can be set by sysvar.
     *
     * We only support a delimiter up to 32 chars
     */
    char * m_msg_delimiter;

    /**
     * Configuration of yajl. Leave public so sysvar can update this directly.
     */
    yajl_gen_config config;
    
protected:

	Audit_json_formatter& operator =(const Audit_json_formatter& b);
	Audit_json_formatter(const Audit_json_formatter& );
	
	/**
	 * Boolean indicating if password masking regex is compiled
	 */
	my_bool m_password_mask_regex_compiled;
	
	/**
	 * Regex used for password masking
	 */
	pcre * m_password_mask_regex_preg;
		
};

/**
 * Base class for audit handlers. Provides basic locking setup.
 */
class Audit_handler
{
public:



    static const size_t MAX_AUDIT_HANDLERS_NUM = 4;
    static const size_t JSON_FILE_HANDLER = 1;
    static const size_t JSON_SOCKET_HANDLER = 3;

    static Audit_handler * m_audit_handler_list[];

    /**
     * Will iterate the handler list and log using each handler
     */
    static void log_audit_all(ThdSesData *pThdData);

    /**
     * Will iterate the handler list and stop all handlers
     */
    static void stop_all();

    Audit_handler() :
        m_initialized(false), m_enabled(false), m_print_offset_err(true), m_formatter(NULL), m_failed(false), m_log_io_errors(true)
    {
    }

    virtual ~Audit_handler()
    {
        if (m_initialized)
        {
            rwlock_destroy(&LOCK_audit);
			pthread_mutex_destroy(&LOCK_io);
        }
    }

    /**
     * Should be called to initialize. We don't init in constructor in order to provide indication if
     * pthread stuff failed init.
     *
     * @frmt the formatter to use in this handler (does not manage distruction of this object)
     * @return 0 on success
     */
    int init(Audit_formatter * frmt)
    {
        m_formatter = frmt;
        if (m_initialized)
        {
            return 0;
        }
        int res = my_rwlock_init(&LOCK_audit, NULL);
        if (res)
        {
            return res;
        }
        res = pthread_mutex_init(&LOCK_io, MY_MUTEX_INIT_SLOW);;
        if (res)
        {
            return res;
        }
        m_initialized = true;
        return res;
    }

    bool is_init()
    {
        return m_initialized;
    }

    void set_enable(bool val);
	
	bool is_enabled()
	{
		return m_enabled;
	}
	
	/**
	 * will close and start the handler
	 */
	void flush();

    /**
     * Will get relevant shared lock and call internal method of handler
     */
    void log_audit(ThdSesData *pThdData);		
	
	/**
	 * Public so can be configured via sysvar
	 */
	unsigned int m_retry_interval;

protected:
    Audit_formatter * m_formatter;
    virtual void handler_start();
	//wiil call internal method and set failed as needed
	bool handler_start_nolock();
    virtual void handler_stop();    
	virtual bool handler_start_internal() = 0;
	virtual void handler_stop_internal() = 0;
    virtual bool handler_log_audit(ThdSesData *pThdData) =0;
    bool m_initialized;
    bool m_enabled;
	bool m_failed;
	bool m_log_io_errors;
	time_t m_last_retry_sec_ts;	    
	inline void set_failed()
	{
		time(&m_last_retry_sec_ts);
		m_failed = true;
		m_log_io_errors = false;
	}
	inline bool is_failed_now()
	{
		return m_failed && (m_retry_interval < 0 || 
			difftime(time(NULL), m_last_retry_sec_ts) > m_retry_interval);
	}
    //override default assignment and copy to protect against creating additional instances
	Audit_handler & operator=(const Audit_handler&);
	Audit_handler(const Audit_handler&);	
private:
    //bool indicating if to print offset errors to log or not
    bool m_print_offset_err;
	//lock io
	pthread_mutex_t LOCK_io;
	//audit (enable) lock
	rw_lock_t LOCK_audit;
    inline void lock_shared()
    {
        rw_rdlock(&LOCK_audit);
    }
    inline void lock_exclusive()
    {
        rw_wrlock(&LOCK_audit);
    }
    inline void unlock()
    {
        rw_unlock(&LOCK_audit);
    }
};

/**
 * Base class for handler which have io and need a lock
 */
class Audit_io_handler: public Audit_handler, public IWriter
{
public:
	Audit_io_handler() : m_io_dest(NULL), m_io_type(NULL)
	{
	}
	
    virtual ~Audit_io_handler() 
    {        
    }
		
	
	/**
	 * target we write to (socket/file). Public so we update via sysvar
	 */
	char * m_io_dest;

protected:
    virtual bool handler_start_internal();
	virtual void handler_stop_internal();
	//used for logging messages
	const char * m_io_type;
};

class Audit_file_handler: public Audit_io_handler
{
public:

    Audit_file_handler() :
        m_sync_period(0), m_log_file(NULL), m_sync_counter(0), m_bufsize(0)
    {
		m_io_type = "file";
    }

    virtual ~Audit_file_handler()
    {
    }

    /**
     * The period to use for syncing to the file system. 0 means we don't sync.
     * 1 means each write we sync. Larger than 1 means every sync_period we sync.
     *
     * We leave this public so the mysql sysvar function can update this variable directly.
     */
    unsigned int m_sync_period;
    
    /**
     * The buf size used by the file stream. 0 = use default, negative or 1 = no buffering
     */
    long m_bufsize;
    
    /**
     * Write function we pass to formatter
     */
    ssize_t write(const char * data, size_t size);
	
	void close();
    
	int open(const char * io_dest, bool m_log_errors);
    //static void print_sleep (THD *thd, int delay_ms);
protected:
    //override default assignment and copy to protect against creating additional instances
	Audit_file_handler & operator=(const Audit_file_handler&);
	Audit_file_handler(const Audit_file_handler&);
    
    

    /**
     * Will acquire locks and call handler_write
     */
    virtual bool handler_log_audit(ThdSesData *pThdData);
    FILE * m_log_file;
    //the period to use for syncing
    unsigned int m_sync_counter;
    

};

class Audit_socket_handler: public Audit_io_handler
{
public:

    Audit_socket_handler() :
        m_vio(NULL), m_connect_timeout(1)
    {
		m_io_type = "socket";
    }

    virtual ~Audit_socket_handler()
    {
    }

    
    /**
     * Connect timeout in secconds
     */
    unsigned int m_connect_timeout;

    /**
     * Write function we pass to formatter
     */
    ssize_t write(const char * data, size_t size);
	
	void close();
    
	int open(const char * io_dest, bool log_errors);
protected:
    //override default assignment and copy to protect against creating additional instances
	Audit_socket_handler & operator=(const Audit_socket_handler&);
	Audit_socket_handler(const Audit_socket_handler&);        

    /**
     * Will acquire locks and call handler_write
     */
    virtual bool handler_log_audit(ThdSesData *pThdData);
    //Vio we write to
    //define as void* so we don't access members directly
    void * m_vio;    
};

#endif /* AUDIT_HANDLER_H_ */

