goog.provide('rethinkdb.Database');

goog.require('rethinkdb');

/**
 * Construct a new metaquery node of the given type on the given database
 * @class A query to perform operations on top level databases
 * @param {MetaQuery.MetaQueryType} type The metaquery type to perform
 * @param {string=} opt_dbName The database to perform this query on
 * @constructor
 * @extends {rethinkdb.Query}
 * @ignore
 */
rethinkdb.MetaQuery = function(type, opt_dbName) {
    this.type_ = type;
    this.dbName_ = opt_dbName || null;
};
goog.inherits(rethinkdb.MetaQuery, rethinkdb.Query);

/** @override */
rethinkdb.MetaQuery.prototype.buildQuery = function() {
    var meta = new MetaQuery();
    meta.setType(this.type_);
    if (this.dbName_) {
        meta.setDbName(this.dbName_);
    }

    var query = new Query();
    query.setType(Query.QueryType.META);
    query.setMetaQuery(meta);

    return query;
};

/**
 * Create a new database with the given name.
 * @param {string} dbName
 * @export
 */
rethinkdb.dbCreate = function(dbName) {
    argCheck_(arguments, 1);
    typeCheck_(dbName, 'string');
    return new rethinkdb.MetaQuery(MetaQuery.MetaQueryType.CREATE_DB, dbName);
};

/**
 * Drop the given database.
 * @param {string} dbName
 * @export
 */
rethinkdb.dbDrop = function(dbName) {
    argCheck_(arguments, 1);
    typeCheck_(dbName, 'string');
    return new rethinkdb.MetaQuery(MetaQuery.MetaQueryType.DROP_DB, dbName);
};

/**
 * List all databases.
 * @export
 */
rethinkdb.dbList = function() {
    return new rethinkdb.MetaQuery(MetaQuery.MetaQueryType.LIST_DBS);
};

/**
 * @class A reference to a database.
 * @param {string} dbName
 * @constructor
 */
rethinkdb.Database = function(dbName) {
    this.name_ = dbName;
};

/**
 * Construct a database reference.
 * @param {string} dbName
 * @return {rethinkdb.Database}
 * @export
 */
rethinkdb.db = function(dbName) {
    argCheck_(arguments, 1);
    typeCheck_(dbName, 'string');
    return new rethinkdb.Database(dbName);
};

/**
 * List all tables in this database
 */
rethinkdb.Database.prototype.list = function() {
    return new rethinkdb.MetaQuery(MetaQuery.MetaQueryType.LIST_TABLES, this.name_);
};
goog.exportProperty(rethinkdb.Database.prototype, 'list',
                    rethinkdb.Database.prototype.list);

/**
 * @class A query that creates a table in a database
 * @param {string} dataCenter
 * @param {string} dbName
 * @param {string} tableName
 * @param {string=} opt_primaryKey
 * @constructor
 * @extends {rethinkdb.Query}
 * @ignore
 */
rethinkdb.CreateTableQuery = function(dataCenter, dbName, tableName, opt_primaryKey) {
    this.dataCenter_ = dataCenter;
    this.dbName_ = dbName;
    this.tableName_ = tableName;
    this.primaryKey_ = opt_primaryKey || 'id';
};
goog.inherits(rethinkdb.CreateTableQuery, rethinkdb.Query);

/** @override */
rethinkdb.CreateTableQuery.prototype.buildQuery = function() {
    var tableref = new TableRef();
    tableref.setDbName(this.dbName_);
    tableref.setTableName(this.tableName_);

    var createtable = new MetaQuery.CreateTable();
    createtable.setDatacenter(this.dataCenter_);
    createtable.setTableRef(tableref);
    createtable.setPrimaryKey(this.primaryKey_);

    var meta = new MetaQuery();
    meta.setType(MetaQuery.MetaQueryType.CREATE_TABLE);
    meta.setCreateTable(createtable);

    var query = new Query();
    query.setType(Query.QueryType.META);
    query.setMetaQuery(meta);

    return query;
};

/**
 * Create a new table in the database
 * @param {string} tableName
 * @param {string=} opt_primaryKey
 */
rethinkdb.Database.prototype.create = function(tableName, opt_primaryKey) {
    argCheck_(arguments, 1);
    typeCheck_(tableName, 'string');
    typeCheck_(opt_primaryKey, 'string');
    return new rethinkdb.CreateTableQuery('Welcome-dc', this.name_, tableName, opt_primaryKey);
};
goog.exportProperty(rethinkdb.Database.prototype, 'create',
                    rethinkdb.Database.prototype.create);

/**
 * @class A query that drops a table from a database
 * @param {string} dbName
 * @param {string} tableName
 * @constructor
 * @extends {rethinkdb.Query}
 * @ignore
 */
rethinkdb.DropTableQuery = function(dbName, tableName) {
    argCheck_(arguments, 1);
    this.dbName_ = dbName;
    this.tableName_ = tableName;
};
goog.inherits(rethinkdb.DropTableQuery, rethinkdb.Query);

/** @override */
rethinkdb.DropTableQuery.prototype.buildQuery = function() {
    var tableref = new TableRef();
    tableref.setDbName(this.dbName_);
    tableref.setTableName(this.tableName_);

    var meta = new MetaQuery();
    meta.setType(MetaQuery.MetaQueryType.DROP_TABLE);
    meta.setDropTable(tableref);

    var query = new Query();
    query.setType(Query.QueryType.META);
    query.setMetaQuery(meta);

    return query;
};

/**
 * Drop a table from this database
 * @param {string} tableName
 */
rethinkdb.Database.prototype.drop = function(tableName) {
    argCheck_(arguments, 1);
    typeCheck_(tableName, 'string');
    return new rethinkdb.DropTableQuery(this.name_, tableName);
};
goog.exportProperty(rethinkdb.Database.prototype, 'drop',
                    rethinkdb.Database.prototype.drop);

/**
 * Construct a table reference for a table in this database
 * @param {string} tableName
 */
rethinkdb.Database.prototype.table = function(tableName) {
    argCheck_(arguments, 1);
    typeCheck_(tableName, 'string');
    return new rethinkdb.Table(tableName, this.name_);
};
goog.exportProperty(rethinkdb.Database.prototype, 'table',
                    rethinkdb.Database.prototype.table);