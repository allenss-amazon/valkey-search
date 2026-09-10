"""Integration tests for what a query asks the database for.

A query is in exactly one of three states, and they are held apart by the pair
(`all_content`, `return_attributes`) rather than by one field's emptiness:

  * nothing        -- FT.SEARCH NOCONTENT or RETURN 0; FT.AGGREGATE with no
                      LOAD clause, or LOAD 0
  * a named list   -- FT.SEARCH RETURN n ...; FT.AGGREGATE LOAD n ...
  * the whole record -- a bare FT.SEARCH; FT.AGGREGATE LOAD *

The third used to be spelled as "nothing was named", which left no way to say
"the whole record *and* these fields". That combination is exactly what a JSON
index needs: `LOAD *` fetches the root document under `$`, which satisfies no
`@field` reference, so a pipeline stage naming one needs its path fetched
alongside the root. Hence the JSON GROUPBY/REDUCE cases below.

Every variant is exercised on both key types, because the whole-record request
is spelled differently for each -- every field for HASH, the `$` root for JSON.
"""

import json
import struct

import pytest
from valkey.client import Valkey
from valkey_search_test_case import ValkeySearchTestCaseDebugMode
from valkeytestframework.conftest import resource_port_tracker


# Naming a loaded JSON column by the field token rather than by the schema
# identifier (issue #1243) is gated behind `search.emulate-release`; these
# tests lift the ceiling so the field names read the way Redis writes them.
FIX_RELEASE = "1.3.0"

DOCS = [
    # key suffix, cat, price, qty
    ("1", "a", 10, 1),
    ("2", "b", 20, 2),
    ("3", "a", 30, 3),
    ("4", "b", 40, 4),
]


def agg_rows(reply):
    """An FT.AGGREGATE reply as a list of {field: value} dicts."""
    out = []
    for row in reply[1:]:
        out.append({row[i].decode(): row[i + 1] for i in range(0, len(row), 2)})
    return out


def search_rows(reply):
    """An FT.SEARCH reply as a list of (key, {field: value}) pairs.

    NOCONTENT replies carry no field array, which is the thing under test, so
    the shape is reported rather than normalised away.
    """
    out = []
    i = 1
    while i < len(reply):
        key = reply[i]
        if i + 1 < len(reply) and isinstance(reply[i + 1], list):
            fields = reply[i + 1]
            out.append((key, {fields[j].decode(): fields[j + 1]
                              for j in range(0, len(fields), 2)}))
            i += 2
        else:
            out.append((key, None))   # no field array at all
            i += 1
    return out


@pytest.mark.parametrize("key_type", ["hash", "json"])
class TestContentRequest(ValkeySearchTestCaseDebugMode):
    """The three states, on both key types."""

    def _client(self, key_type) -> Valkey:
        client: Valkey = self.server.get_new_client()
        assert client.execute_command(
            f"CONFIG SET search.emulate-release {FIX_RELEASE}") == b"OK"
        if key_type == "hash":
            client.execute_command(
                "FT.CREATE", "idx", "ON", "HASH", "PREFIX", "1", "d:",
                "SCHEMA", "cat", "TAG", "price", "NUMERIC", "qty", "NUMERIC",
            )
            for suffix, cat, price, qty in DOCS:
                client.execute_command(
                    "HSET", f"d:{suffix}", "cat", cat,
                    "price", str(price), "qty", str(qty))
        else:
            client.execute_command(
                "FT.CREATE", "idx", "ON", "JSON", "PREFIX", "1", "d:",
                "SCHEMA",
                "$.cat", "AS", "cat", "TAG",
                "$.price", "AS", "price", "NUMERIC",
                "$.qty", "AS", "qty", "NUMERIC",
            )
            for suffix, cat, price, qty in DOCS:
                client.execute_command(
                    "JSON.SET", f"d:{suffix}", "$",
                    json.dumps({"cat": cat, "price": price, "qty": qty}))
        return client

    # ----- FT.AGGREGATE -------------------------------------------------

    def test_aggregate_nothing(self, key_type):
        """No LOAD clause, and LOAD 0, both ask for nothing: the rows come
        back with no fields at all."""
        client = self._client(key_type)
        for tail in ([], ["LOAD", "0"]):
            reply = client.execute_command(
                "FT.AGGREGATE", "idx", "@price:[-inf inf]", *tail,
                "LIMIT", "0", "100", "DIALECT", "2")
            rows = agg_rows(reply)
            assert len(rows) == len(DOCS), f"{tail}: {reply}"
            for row in rows:
                assert row == {}, f"{tail} returned fields: {row}"

    def test_aggregate_named_list(self, key_type):
        """LOAD n ... asks for exactly those fields and no others."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]",
            "LOAD", "2", "@cat", "@price",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for row in rows:
            assert set(row) == {"cat", "price"}, row
        assert {int(r["price"]) for r in rows} == {d[2] for d in DOCS}

    def test_aggregate_whole_record(self, key_type):
        """LOAD * asks for the whole record. HASH spells that as every field;
        JSON spells it as the root document under `$`."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for row in rows:
            if key_type == "hash":
                assert set(row) == {"cat", "price", "qty"}, row
            else:
                assert set(row) == {"$"}, row
                doc = json.loads(row["$"])
                assert set(doc) == {"cat", "price", "qty"}, doc

    # ----- FT.SEARCH ----------------------------------------------------

    def test_search_nothing(self, key_type):
        """NOCONTENT and RETURN 0 both reply with keys and no field array."""
        client = self._client(key_type)
        for tail in (["NOCONTENT"], ["RETURN", "0"]):
            rows = search_rows(client.execute_command(
                "FT.SEARCH", "idx", "@price:[-inf inf]", *tail,
                "LIMIT", "0", "100", "DIALECT", "2"))
            assert len(rows) == len(DOCS), f"{tail}"
            for _key, fields in rows:
                assert fields is None, f"{tail} returned a field array: {fields}"

    def test_search_nothing_wins_over_return(self, key_type):
        """NOCONTENT beats a RETURN clause on either side of it."""
        client = self._client(key_type)
        for tail in (["NOCONTENT", "RETURN", "1", "cat"],
                     ["RETURN", "1", "cat", "NOCONTENT"]):
            rows = search_rows(client.execute_command(
                "FT.SEARCH", "idx", "@price:[-inf inf]", *tail,
                "LIMIT", "0", "100", "DIALECT", "2"))
            for _key, fields in rows:
                assert fields is None, f"{tail} returned a field array: {fields}"

    def test_search_named_list(self, key_type):
        """RETURN n ... replies with exactly those fields."""
        client = self._client(key_type)
        rows = search_rows(client.execute_command(
            "FT.SEARCH", "idx", "@price:[-inf inf]", "RETURN", "1", "cat",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for _key, fields in rows:
            assert set(fields) == {"cat"}, fields

    def test_search_whole_record(self, key_type):
        """A bare FT.SEARCH replies with the whole record."""
        client = self._client(key_type)
        rows = search_rows(client.execute_command(
            "FT.SEARCH", "idx", "@price:[-inf inf]",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for _key, fields in rows:
            if key_type == "hash":
                assert set(fields) == {"cat", "price", "qty"}, fields
            else:
                assert set(fields) == {"$"}, fields
                assert set(json.loads(fields["$"])) == {"cat", "price", "qty"}


@pytest.mark.parametrize("key_type", ["hash", "json"])
class TestWholeRecordAutoLoad(ValkeySearchTestCaseDebugMode):
    """A pipeline stage naming a field, under `LOAD *`.

    On HASH this has always worked: the whole record arrives keyed by field
    name, which is what the stage's column looks itself up by. On JSON it did
    not -- the record arrives as one `$` blob under which no `@field` resolves
    -- so `GROUPBY 1 @cat` put every document in a single null-keyed group and
    a `REDUCE SUM 1 @price` summed nothing. The whole-record request now
    coexists with the named paths those stages need.
    """

    def _client(self, key_type) -> Valkey:
        client: Valkey = self.server.get_new_client()
        assert client.execute_command(
            f"CONFIG SET search.emulate-release {FIX_RELEASE}") == b"OK"
        if key_type == "hash":
            client.execute_command(
                "FT.CREATE", "idx", "ON", "HASH", "PREFIX", "1", "d:",
                "SCHEMA", "cat", "TAG", "price", "NUMERIC", "qty", "NUMERIC")
            for suffix, cat, price, qty in DOCS:
                client.execute_command(
                    "HSET", f"d:{suffix}", "cat", cat,
                    "price", str(price), "qty", str(qty))
        else:
            client.execute_command(
                "FT.CREATE", "idx", "ON", "JSON", "PREFIX", "1", "d:",
                "SCHEMA",
                "$.cat", "AS", "cat", "TAG",
                "$.price", "AS", "price", "NUMERIC",
                "$.qty", "AS", "qty", "NUMERIC")
            for suffix, cat, price, qty in DOCS:
                client.execute_command(
                    "JSON.SET", f"d:{suffix}", "$",
                    json.dumps({"cat": cat, "price": price, "qty": qty}))
        return client

    def test_groupby_key_resolves_under_load_star(self, key_type):
        """The GROUPBY key is the field's value, not a single null group."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "GROUPBY", "1", "@cat", "REDUCE", "COUNT", "0", "AS", "n",
            "DIALECT", "2"))
        assert {r["cat"]: int(r["n"]) for r in rows} == {b"a": 2, b"b": 2}

    def test_reduce_argument_resolves_under_load_star(self, key_type):
        """A REDUCE argument names a field too, and has to be fetched for the
        same reason the group key does."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "GROUPBY", "1", "@cat",
            "REDUCE", "SUM", "1", "@price", "AS", "total",
            "REDUCE", "MAX", "1", "@qty", "AS", "peak",
            "DIALECT", "2"))
        got = {r["cat"]: (int(r["total"]), int(r["peak"])) for r in rows}
        # cat a: prices 10+30, qty max 3.  cat b: prices 20+40, qty max 4.
        assert got == {b"a": (40, 3), b"b": (60, 4)}, got

    def test_two_group_keys_resolve_under_load_star(self, key_type):
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "GROUPBY", "2", "@cat", "@qty",
            "REDUCE", "COUNT", "0", "AS", "n",
            "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for row in rows:
            assert row["cat"] in (b"a", b"b"), row
            assert int(row["n"]) == 1, row

    def test_sortby_and_apply_resolve_under_load_star(self, key_type):
        """SORTBY and APPLY name fields the same way GROUPBY does."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "APPLY", "@price * 2", "AS", "double",
            "SORTBY", "2", "@price", "DESC",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert [int(r["price"]) for r in rows] == [40, 30, 20, 10]
        for row in rows:
            assert int(row["double"]) == 2 * int(row["price"]), row

    def test_whole_record_still_returned_alongside(self, key_type):
        """Fetching the named field does not replace the whole record: the
        stage's field and the rest of the document both come back."""
        client = self._client(key_type)
        rows = agg_rows(client.execute_command(
            "FT.AGGREGATE", "idx", "@price:[-inf inf]", "LOAD", "*",
            "SORTBY", "2", "@price", "ASC",
            "LIMIT", "0", "100", "DIALECT", "2"))
        assert len(rows) == len(DOCS)
        for row in rows:
            assert "price" in row, row
            if key_type == "json":
                # The root document is still there next to the fetched path.
                assert "$" in row, row
                assert set(json.loads(row["$"])) == {"cat", "price", "qty"}
            else:
                assert {"cat", "qty"} <= set(row), row


def _vec(*xs: float) -> bytes:
    return struct.pack(f"{len(xs)}f", *xs)


@pytest.mark.parametrize("key_type", ["hash", "json"])
class TestHybridContentRequest(ValkeySearchTestCaseDebugMode):
    """The same three states, reached through FT.HYBRID.

    FT.HYBRID has no NOCONTENT clause; its two arms never fetch anything and
    the trailing aggregate pipeline decides what the reply carries, exactly as
    FT.AGGREGATE does. The document key and the score aliases are not database
    content, so they ride along in every state and are subtracted here.
    """

    Q = _vec(1.0, 0.0, 0.0, 0.0)

    def _client(self, key_type) -> Valkey:
        client: Valkey = self.server.get_new_client()
        assert client.execute_command(
            f"CONFIG SET search.emulate-release {FIX_RELEASE}") == b"OK"
        if key_type == "hash":
            client.execute_command(
                "FT.CREATE", "idx", "ON", "HASH", "PREFIX", "1", "d:",
                "SCHEMA", "title", "TEXT", "NOSTEM", "cat", "TAG",
                "price", "NUMERIC", "qty", "NUMERIC",
                "vec", "VECTOR", "HNSW", "6", "TYPE", "FLOAT32",
                "DIM", "4", "DISTANCE_METRIC", "L2")
            for i, (suffix, cat, price, qty) in enumerate(DOCS):
                client.execute_command(
                    "HSET", f"d:{suffix}", "title", "hello world", "cat", cat,
                    "price", str(price), "qty", str(qty),
                    "vec", _vec(1.0 + i, 0.0, 0.0, 0.0))
        else:
            client.execute_command(
                "FT.CREATE", "idx", "ON", "JSON", "PREFIX", "1", "d:",
                "SCHEMA",
                "$.title", "AS", "title", "TEXT", "NOSTEM",
                "$.cat", "AS", "cat", "TAG",
                "$.price", "AS", "price", "NUMERIC",
                "$.qty", "AS", "qty", "NUMERIC",
                "$.vec", "AS", "vec", "VECTOR", "HNSW", "6", "TYPE", "FLOAT32",
                "DIM", "4", "DISTANCE_METRIC", "L2")
            for i, (suffix, cat, price, qty) in enumerate(DOCS):
                client.execute_command(
                    "JSON.SET", f"d:{suffix}", "$", json.dumps({
                        "title": "hello world", "cat": cat, "price": price,
                        "qty": qty, "vec": [1.0 + i, 0.0, 0.0, 0.0]}))
        return client

    def _rows(self, client, *extra):
        return agg_rows(client.execute_command(
            "FT.HYBRID", "idx",
            "SEARCH", "@title:hello",
            "VSIM", "@vec", "$q", "KNN", "2", "K", "10",
            "COMBINE", "RRF", "2", "YIELD_SCORE_AS", "hs",
            *extra,
            "LIMIT", "0", "100",
            "PARAMS", "2", "q", self.Q))

    def _content(self, row):
        return set(row) - {"__key", "hs"}

    def test_hybrid_nothing(self, key_type):
        """No LOAD clause, and LOAD 0: no database field is fetched."""
        client = self._client(key_type)
        for tail in ([], ["LOAD", "0"]):
            rows = self._rows(client, *tail)
            assert len(rows) == len(DOCS), f"{tail}: {rows}"
            for row in rows:
                assert self._content(row) == set(), f"{tail}: {row}"

    def test_hybrid_named_list(self, key_type):
        client = self._client(key_type)
        rows = self._rows(client, "LOAD", "2", "@cat", "@price")
        assert len(rows) == len(DOCS)
        for row in rows:
            assert self._content(row) == {"cat", "price"}, row
        assert {int(r["price"]) for r in rows} == {d[2] for d in DOCS}

    def test_hybrid_whole_record(self, key_type):
        client = self._client(key_type)
        rows = self._rows(client, "LOAD", "*")
        assert len(rows) == len(DOCS)
        for row in rows:
            if key_type == "hash":
                assert {"title", "cat", "price", "qty"} <= self._content(row), row
            else:
                assert self._content(row) == {"$"}, row
                assert {"cat", "price", "qty"} <= set(json.loads(row["$"]))

    def test_hybrid_groupby_reduce_under_load_star(self, key_type):
        """The pipeline stage's fields resolve under `LOAD *` here too."""
        client = self._client(key_type)
        rows = self._rows(
            client, "LOAD", "*",
            "GROUPBY", "1", "@cat",
            "REDUCE", "COUNT", "0", "AS", "n",
            "REDUCE", "SUM", "1", "@price", "AS", "total",
            "REDUCE", "MAX", "1", "@qty", "AS", "peak")
        got = {r["cat"]: (int(r["n"]), int(r["total"]), int(r["peak"]))
               for r in rows}
        assert got == {b"a": (2, 40, 3), b"b": (2, 60, 4)}, got
