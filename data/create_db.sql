create table messages (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	mode TEXT,
	freq INTEGER,
	qso_date INTEGER,
	qso_time INTEGER,
	is_outgoing INTEGER DEFAULT 0,	
	data TEXT DEFAULT ""
);

create table contacts(
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	callsign TEXT,
	name TEXT,
	last_seen_on INTEGER,
	status TEXT
);


create table logbook (
	id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	mode TEXT,
	freq TEXT,
	qso_date TEXT,
	qso_time TEXT,
	callsign_sent TEXT,
	rst_sent TEXT,
	exch_sent TEXT DEFAULT "",
	callsign_recv TEXT,
	rst_recv TEXT,
	exch_recv TEXT DEFAULT "",
	tx_id	TEXT DEFAULT "",
	comments TEXT DEFAULT ""
);
CREATE INDEX gridIx ON logbook (exch_recv);
CREATE INDEX callIx ON logbook (callsign_recv);

