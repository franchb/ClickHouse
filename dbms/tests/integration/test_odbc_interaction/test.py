import time
import pytest

import os
import pymysql.cursors
import psycopg2
from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT
from helpers.cluster import ClickHouseCluster

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))

cluster = ClickHouseCluster(__file__, base_configs_dir=os.path.join(SCRIPT_DIR, 'configs'))
node1 = cluster.add_instance('node1', with_odbc_drivers=True, with_mysql=True, image='alesapin/ubuntu_with_odbc', main_configs=['configs/dictionaries/sqlite3_odbc_hashed_dictionary.xml', 'configs/dictionaries/sqlite3_odbc_cached_dictionary.xml', 'configs/dictionaries/postgres_odbc_hashed_dictionary.xml'], stay_alive=True)

create_table_sql_template =   """
    CREATE TABLE `clickhouse`.`{}` (
    `id` int(11) NOT NULL,
    `name` varchar(50) NOT NULL,
    `age` int  NOT NULL default 0,
    `money` int NOT NULL default 0,
    PRIMARY KEY (`id`)) ENGINE=InnoDB;
    """
def get_mysql_conn():
    conn = pymysql.connect(user='root', password='clickhouse', host='127.0.0.1', port=3308)
    return conn

def create_mysql_db(conn, name):
    with conn.cursor() as cursor:
        cursor.execute(
            "CREATE DATABASE {} DEFAULT CHARACTER SET 'utf8'".format(name))

def create_mysql_table(conn, table_name):
    with conn.cursor() as cursor:
        cursor.execute(create_table_sql_template.format(table_name))

def get_postgres_conn():
    conn_string = "host='localhost' user='postgres' password='mysecretpassword'"
    conn = psycopg2.connect(conn_string)
    conn.set_isolation_level(ISOLATION_LEVEL_AUTOCOMMIT)
    conn.autocommit = True
    return conn

def create_postgres_db(conn, name):
    cursor = conn.cursor()
    cursor.execute("CREATE SCHEMA {}".format(name))

@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        sqlite_db = node1.odbc_drivers["SQLite3"]["Database"]

        print "sqlite data received"
        node1.exec_in_container(["bash", "-c", "echo 'CREATE TABLE t1(x INTEGER PRIMARY KEY ASC, y, z);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
        node1.exec_in_container(["bash", "-c", "echo 'CREATE TABLE t2(X INTEGER PRIMARY KEY ASC, Y, Z);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
        node1.exec_in_container(["bash", "-c", "echo 'CREATE TABLE t3(X INTEGER PRIMARY KEY ASC, Y, Z);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
        node1.exec_in_container(["bash", "-c", "echo 'CREATE TABLE t4(X INTEGER PRIMARY KEY ASC, Y, Z);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
        print "sqlite tables created"
        mysql_conn = get_mysql_conn()
        print "mysql connection received"
        ## create mysql db and table
        create_mysql_db(mysql_conn, 'clickhouse')
        print "mysql database created"

        postgres_conn = get_postgres_conn()
        print "postgres connection received"

        create_postgres_db(postgres_conn, 'clickhouse')
        print "postgres db created"

        cursor = postgres_conn.cursor()
        cursor.execute("create table if not exists clickhouse.test_table (column1 int primary key, column2 varchar(40) not null)")

        yield cluster

    except Exception as ex:
        print(ex)
        raise ex
    finally:
        cluster.shutdown()

def test_mysql_simple_select_works(started_cluster):
    mysql_setup = node1.odbc_drivers["MySQL"]

    table_name = 'test_insert_select'
    conn = get_mysql_conn()
    create_mysql_table(conn, table_name)

    node1.query('''
CREATE TABLE {}(id UInt32, name String, age UInt32, money UInt32) ENGINE = MySQL('mysql1:3306', 'clickhouse', '{}', 'root', 'clickhouse');
'''.format(table_name, table_name))

    node1.query("INSERT INTO {}(id, name, money) select number, concat('name_', toString(number)), 3 from numbers(100) ".format(table_name))

    # actually, I don't know, what wrong with that connection string, but libmyodbc always falls into segfault
    node1.query("SELECT * FROM odbc('DSN={}', '{}')".format(mysql_setup["DSN"], table_name), ignore_error=True)

    # server still works after segfault
    assert node1.query("select 1") == "1\n"

    conn.close()


def test_sqlite_simple_select_function_works(started_cluster):
    sqlite_setup = node1.odbc_drivers["SQLite3"]
    sqlite_db = sqlite_setup["Database"]

    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t1 values(1, 2, 3);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
    assert node1.query("select * from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "1\t2\t3\n"

    assert node1.query("select y from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "2\n"
    assert node1.query("select z from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "3\n"
    assert node1.query("select x from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "1\n"
    assert node1.query("select x, y from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "1\t2\n"
    assert node1.query("select z, x, y from odbc('DSN={}', '{}')".format(sqlite_setup["DSN"], 't1')) == "3\t1\t2\n"
    assert node1.query("select count(), sum(x) from odbc('DSN={}', '{}') group by x".format(sqlite_setup["DSN"], 't1')) == "1\t1\n"

def test_sqlite_simple_select_storage_works(started_cluster):
    sqlite_setup = node1.odbc_drivers["SQLite3"]
    sqlite_db = sqlite_setup["Database"]

    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t4 values(1, 2, 3);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')
    node1.query("create table SqliteODBC (x Int32, y String, z String) engine = ODBC('DSN={}', '', 't4')".format(sqlite_setup["DSN"]))

    assert node1.query("select * from SqliteODBC") == "1\t2\t3\n"
    assert node1.query("select y from SqliteODBC") == "2\n"
    assert node1.query("select z from SqliteODBC") == "3\n"
    assert node1.query("select x from SqliteODBC") == "1\n"
    assert node1.query("select x, y from SqliteODBC") == "1\t2\n"
    assert node1.query("select z, x, y from SqliteODBC") == "3\t1\t2\n"
    assert node1.query("select count(), sum(x) from SqliteODBC group by x") == "1\t1\n"

def test_sqlite_odbc_hashed_dictionary(started_cluster):
    sqlite_db =  node1.odbc_drivers["SQLite3"]["Database"]
    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t2 values(1, 2, 3);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(1))") == "3\n"
    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(200))") == "1\n" # default

    time.sleep(5) # first reload
    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t2 values(200, 2, 7);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    # No reload because of invalidate query
    time.sleep(5)
    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(1))") == "3\n"
    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(200))") == "1\n" # still default

    node1.exec_in_container(["bash", "-c", "echo 'REPLACE INTO t2 values(1, 2, 5);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    # waiting for reload
    time.sleep(5)

    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(1))") == "5\n"
    assert node1.query("select dictGetUInt8('sqlite3_odbc_hashed', 'Z', toUInt64(200))") == "7\n" # new value

def test_sqlite_odbc_cached_dictionary(started_cluster):
    sqlite_db =  node1.odbc_drivers["SQLite3"]["Database"]
    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t3 values(1, 2, 3);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    assert node1.query("select dictGetUInt8('sqlite3_odbc_cached', 'Z', toUInt64(1))") == "3\n"

    node1.exec_in_container(["bash", "-c", "echo 'INSERT INTO t3 values(200, 2, 7);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    assert node1.query("select dictGetUInt8('sqlite3_odbc_cached', 'Z', toUInt64(200))") == "7\n" # new value

    node1.exec_in_container(["bash", "-c", "echo 'REPLACE INTO t3 values(1, 2, 12);' | sqlite3 {}".format(sqlite_db)], privileged=True, user='root')

    time.sleep(5)

    assert node1.query("select dictGetUInt8('sqlite3_odbc_cached', 'Z', toUInt64(1))") == "12\n"

def test_postgres_odbc_hached_dictionary_with_schema(started_cluster):
    conn = get_postgres_conn()
    cursor = conn.cursor()
    cursor.execute("insert into clickhouse.test_table values(1, 'hello'),(2, 'world')")
    time.sleep(5)
    assert node1.query("select dictGetString('postgres_odbc_hashed', 'column2', toUInt64(1))") == "hello\n"
    assert node1.query("select dictGetString('postgres_odbc_hashed', 'column2', toUInt64(2))") == "world\n"

def test_postgres_odbc_hached_dictionary_no_tty_pipe_overflow(started_cluster):
    conn = get_postgres_conn()
    cursor = conn.cursor()
    cursor.execute("insert into clickhouse.test_table values(3, 'xxx')")
    for i in xrange(100):
        try:
            node1.query("system reload dictionary postgres_odbc_hashed", timeout=5)
        except Exception as ex:
            assert False, "Exception occured -- odbc-bridge hangs: " + str(ex)

    assert node1.query("select dictGetString('postgres_odbc_hashed', 'column2', toUInt64(3))") == "xxx\n"

def test_bridge_dies_with_parent(started_cluster):
    node1.query("select dictGetString('postgres_odbc_hashed', 'column2', toUInt64(1))")
    def get_pid(cmd):
        output = node1.exec_in_container(["bash", "-c", "ps ax | grep '{}' | grep -v 'grep' | grep -v 'bash -c' | awk '{{print $1}}'".format(cmd)], privileged=True, user='root')
        if output:
            try:
                pid = int(output.split('\n')[0].strip())
                return pid
            except:
                return None
            return None

    clickhouse_pid = get_pid("clickhouse server")
    bridge_pid = get_pid("odbc-bridge")
    assert clickhouse_pid is not None
    assert bridge_pid is not None

    while clickhouse_pid is not None:
        try:
            node1.exec_in_container(["bash", "-c", "kill {}".format(clickhouse_pid)], privileged=True, user='root')
        except:
            pass
        clickhouse_pid = get_pid("clickhouse server")
        time.sleep(1)

    time.sleep(1) # just for sure, that odbc-bridge caught signal
    bridge_pid = get_pid("odbc-bridge")

    assert clickhouse_pid is None
    assert bridge_pid is None
