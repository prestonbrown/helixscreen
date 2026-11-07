# SPDX-License-Identifier: GPL-3.0-or-later
"""
Unit tests for the helix_print Moonraker plugin.

These tests verify the plugin's core functionality without requiring
a running Moonraker instance. They use mocks to simulate Moonraker's
server, file manager, and history components.

Run with: pytest tests/test_helix_print.py -v
"""

import asyncio
import json
import os
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, Optional
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

# Import the plugin (adjust path as needed)
import sys
sys.path.insert(0, str(Path(__file__).parent.parent))

from helix_print import (
    DB_RECORD_MAX_AGE,
    HELIX_NAMESPACE,
    HelixPrint,
    PrintInfo,
    load_component,
)


# ============================================================================
# Test Fixtures and Mocks
# ============================================================================

class MockWebRequest:
    """Mock WebRequest for testing API endpoints."""

    def __init__(self, params: Dict[str, Any]):
        self._params = params

    def get_str(self, key: str, default: str = "") -> str:
        return str(self._params.get(key, default))

    def get_list(self, key: str, default: list = None) -> list:
        return self._params.get(key, default or [])

    def get_boolean(self, key: str, default: bool = False) -> bool:
        return bool(self._params.get(key, default))


class MockServer:
    """Mock Moonraker server for testing."""

    def __init__(self):
        self.endpoints = {}
        self.event_handlers = {}
        self.components = {}
        self._error_class = Exception

    def register_endpoint(self, path: str, methods: list, handler):
        self.endpoints[path] = handler

    def register_event_handler(self, event: str, callback):
        if event not in self.event_handlers:
            self.event_handlers[event] = []
        self.event_handlers[event].append(callback)

    def lookup_component(self, name: str, default=None):
        return self.components.get(name, default)

    def get_event_loop(self):
        return MockEventLoop()

    def error(self, message: str, code: int = 500):
        return Exception(f"{code}: {message}")


class MockEventLoop:
    """Mock event loop for testing."""

    def register_callback(self, callback, *args):
        pass

    def delay_callback(self, delay: float, callback, *args):
        pass

    def run_in_thread(self, callback, *args):
        return asyncio.get_event_loop().run_in_executor(None, callback, *args)


class MockFileManager:
    """Mock file manager for testing."""

    def __init__(self, gcodes_path: str):
        self._gcodes_path = gcodes_path

    def get_directory(self, name: str) -> str:
        if name == "gcodes":
            return self._gcodes_path
        return ""


class MockDatabase:
    """Mock database component for testing.

    Mirrors the real Moonraker v0.10.0 `database` component's public surface
    (verified from Moonraker source commit d5ee171): sql_execute, insert_item,
    get_item, update_item, delete_item, ns_items, ns_length. There is NO
    `execute_db_command` - that API was renamed/removed upstream. Only defining
    the real methods means calling anything else raises AttributeError, same as
    it would against the production object.
    """

    def __init__(self):
        self.namespaces: Dict[str, Dict[str, Any]] = {}
        self.sql_commands = []

    async def sql_execute(self, sql: str, params: Optional[list] = None):
        self.sql_commands.append((sql, params))
        return MagicMock(lastrowid=1, rowcount=0)

    @staticmethod
    def _key(key: Any) -> str:
        # A one-element list names a key whole, as Moonraker takes it.
        return ".".join(key) if isinstance(key, list) else key

    async def insert_item(self, namespace: str, key: Any, value: Any) -> None:
        self.namespaces.setdefault(namespace, {})[self._key(key)] = value

    async def get_item(self, namespace: str, key: Any = None, default: Any = None) -> Any:
        if key is None:
            if namespace not in self.namespaces:
                return default
            return dict(self.namespaces[namespace])
        return self.namespaces.get(namespace, {}).get(self._key(key), default)

    async def update_item(self, namespace: str, key: Any, value: Any) -> None:
        self.namespaces.setdefault(namespace, {})[self._key(key)] = value

    async def delete_item(self, namespace: str, key: Any) -> None:
        self.namespaces.get(namespace, {}).pop(self._key(key), None)

    async def ns_items(self, namespace: str) -> list:
        return list(self.namespaces.get(namespace, {}).items())

    async def ns_length(self, namespace: str) -> int:
        return len(self.namespaces.get(namespace, {}))


class MockKlippy:
    """Mock klippy_connection component for testing.

    Mirrors the real Moonraker v0.10.0 `KlippyConnection`'s public async surface:
    request(web_request) and rollover_log(). It has NO `run_gcode` - that method
    lives on klippy_apis (see MockKlippyApis below). Calling run_gcode on this
    mock raises AttributeError, reproducing the production crash reported in
    debug bundle RA6EPJTZ ("'KlippyConnection' object has no attribute
    'run_gcode'").
    """

    def __init__(self):
        self.requests_sent = []

    async def request(self, web_request):
        self.requests_sent.append(web_request)
        return {}

    async def rollover_log(self):
        pass


class MockKlippyApis:
    """Mock klippy_apis component for testing.

    Mirrors the real Moonraker v0.10.0 `KlippyAPI`'s public async surface
    (verified from Moonraker source commit d5ee171): run_gcode, start_print,
    do_restart, pause_print, resume_print, cancel_print, emergency_stop,
    query_objects, get_object_list, list_endpoints, subscribe_objects,
    get_klippy_info. This is the correct component for Klipper interaction
    (SDCARD_PRINT_FILE, RESTART, etc.) - not klippy_connection.
    """

    def __init__(self):
        self.run_gcode = AsyncMock(return_value="ok")
        self.start_print = AsyncMock(return_value=None)
        self.do_restart = AsyncMock(return_value=None)
        self.pause_print = AsyncMock(return_value=None)
        self.resume_print = AsyncMock(return_value=None)
        self.cancel_print = AsyncMock(return_value=None)
        self.emergency_stop = AsyncMock(return_value=None)
        self.query_objects = AsyncMock(return_value={})
        self.get_object_list = AsyncMock(return_value=[])
        self.list_endpoints = AsyncMock(return_value={})
        self.subscribe_objects = AsyncMock(return_value={})
        self.get_klippy_info = AsyncMock(return_value={})


class PrinterJob:
    """Stand-in for Moonraker's PrinterJob with the same attribute round-trip.

    HelixPrint resolves PrinterJob from the module that defines the live
    history component, so MockHistory's module has to offer one.
    """

    def __init__(self, data: Dict[str, Any] = None):
        self.user = "No User"
        self.filename = None
        self.status = "in_progress"
        self.start_time = 0.0
        self.end_time = None
        self.print_duration = 0.0
        self.total_duration = 0.0
        self.filament_used = 0.0
        self.metadata: Dict[str, Any] = {}
        self.auxiliary_data: list = []
        self.update_from_ps(data or {})

    def update_from_ps(self, data: Dict[str, Any]) -> None:
        for i in data:
            if hasattr(self, i) and data[i] is not None:
                setattr(self, i, data[i])


class MockHistory:
    """Mock history component for testing.

    Mirrors the real Moonraker v0.10.0 `history` component: get_job(job_id)
    accepts a hex string or an int and returns a plain column dict, and
    save_job(job, job_id) takes a PrinterJob plus the numeric id. There is no
    `modify_job` - that method is not part of the component.
    """

    def __init__(self):
        self.jobs: Dict[int, Dict[str, Any]] = {}
        self.save_job_calls = []

    @staticmethod
    def _key(job_id) -> int:
        return int(job_id, 16) if isinstance(job_id, str) else int(job_id)

    def add_job(self, job_id, **columns) -> Dict[str, Any]:
        """Seed a job row with the columns job_history actually carries."""
        key = self._key(job_id)
        job = {
            "job_id": f"{key:06X}",
            "user": "helix",
            "filename": ".helix_print/abcd_benchy.gcode",
            "status": "completed",
            "start_time": 100.0,
            "end_time": 200.0,
            "print_duration": 90.0,
            "total_duration": 100.0,
            "filament_used": 1.5,
            "metadata": {},
            "auxiliary_data": [],
            "instance_id": "default",
        }
        job.update(columns)
        self.jobs[key] = job
        return job

    async def get_job(self, job_id):
        job = self.jobs.get(self._key(job_id))
        return dict(job) if job is not None else None

    async def save_job(self, job, job_id=None):
        self.save_job_calls.append((job, job_id))


class MockConfigHelper:
    """Mock config helper for testing."""

    def __init__(self, server: MockServer, options: Dict[str, Any] = None):
        self._server = server
        self._options = options or {}

    def get_server(self):
        return self._server

    def get(self, key: str, default: str = None) -> str:
        return self._options.get(key, default)

    def getint(self, key: str, default: int = None) -> int:
        return int(self._options.get(key, default))

    def getboolean(self, key: str, default: bool = None) -> bool:
        return bool(self._options.get(key, default))


@pytest.fixture
def temp_gcodes_dir():
    """Create a temporary directory for G-code files."""
    with tempfile.TemporaryDirectory() as tmpdir:
        yield tmpdir


@pytest.fixture
def mock_server():
    """Create a mock Moonraker server."""
    return MockServer()


@pytest.fixture
def helix_print_component(mock_server, temp_gcodes_dir):
    """Create a HelixPrint component instance for testing."""
    # Set up mock components
    mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
    mock_server.components["database"] = MockDatabase()
    mock_server.components["klippy_connection"] = MockKlippy()
    mock_server.components["klippy_apis"] = MockKlippyApis()
    mock_server.components["history"] = MockHistory()

    # Create config
    config = MockConfigHelper(mock_server, {
        "temp_dir": ".helix_temp",
        "symlink_dir": ".helix_print",
        "cleanup_delay": 3600,
        "enabled": True,
    })

    # Create component
    component = load_component(config)
    return component


# ============================================================================
# PrintInfo Tests
# ============================================================================

class TestPrintInfo:
    """Tests for the PrintInfo data class."""

    def test_creation(self):
        """Test PrintInfo can be created with all fields."""
        info = PrintInfo(
            original_filename="benchy.gcode",
            temp_filename=".helix_temp/mod_123_benchy.gcode",
            symlink_filename=".helix_print/benchy.gcode",
            modifications=["bed_leveling_disabled"],
            start_time=1234567890.0,
        )

        assert info.original_filename == "benchy.gcode"
        assert info.temp_filename == ".helix_temp/mod_123_benchy.gcode"
        assert info.symlink_filename == ".helix_print/benchy.gcode"
        assert info.modifications == ["bed_leveling_disabled"]
        assert info.start_time == 1234567890.0
        assert info.job_id is None
        assert info.db_id is None

    def test_job_id_assignment(self):
        """Test job_id can be assigned after creation."""
        info = PrintInfo(
            original_filename="test.gcode",
            temp_filename="temp.gcode",
            symlink_filename="symlink.gcode",
            modifications=[],
            start_time=0.0,
        )

        info.job_id = "ABC123"
        assert info.job_id == "ABC123"


# ============================================================================
# Component Initialization Tests
# ============================================================================

class TestHelixPrintInit:
    """Tests for HelixPrint component initialization."""

    def test_load_component(self, mock_server):
        """Test component loads successfully."""
        config = MockConfigHelper(mock_server)
        component = load_component(config)

        assert component is not None
        assert isinstance(component, HelixPrint)

    def test_default_config(self, mock_server):
        """Test default configuration values."""
        config = MockConfigHelper(mock_server)
        component = load_component(config)

        assert component.temp_dir == ".helix_temp"
        assert component.symlink_dir == ".helix_print"
        assert component.cleanup_delay == 86400  # 24 hours
        assert component.enabled is True

    def test_custom_config(self, mock_server):
        """Test custom configuration values."""
        config = MockConfigHelper(mock_server, {
            "temp_dir": "custom_temp",
            "symlink_dir": "custom_symlink",
            "cleanup_delay": 7200,
            "enabled": False,
        })
        component = load_component(config)

        assert component.temp_dir == "custom_temp"
        assert component.symlink_dir == "custom_symlink"
        assert component.cleanup_delay == 7200
        assert component.enabled is False

    def test_endpoints_registered(self, mock_server):
        """Test API endpoints are registered."""
        config = MockConfigHelper(mock_server)
        load_component(config)

        assert "/server/helix/print_modified" in mock_server.endpoints
        assert "/server/helix/status" in mock_server.endpoints

    def test_event_handlers_registered(self, mock_server):
        """Test event handlers are registered."""
        config = MockConfigHelper(mock_server)
        load_component(config)

        assert "job_state:state_changed" in mock_server.event_handlers
        assert "server:klippy_ready" in mock_server.event_handlers


# ============================================================================
# Status API Tests
# ============================================================================

class TestStatusAPI:
    """Tests for the /server/helix/status endpoint."""

    @pytest.mark.asyncio
    async def test_status_returns_config(self, helix_print_component, mock_server):
        """Test status endpoint returns configuration."""
        handler = mock_server.endpoints["/server/helix/status"]
        request = MockWebRequest({})

        result = await handler(request)

        assert result["enabled"] is True
        assert result["temp_dir"] == ".helix_temp"
        assert result["symlink_dir"] == ".helix_print"
        assert result["cleanup_delay"] == 3600
        assert result["version"] == "1.4.0"
        assert result["active_prints"] == 0


# ============================================================================
# Print Modified API Tests (v2.0 path-based API)
# ============================================================================

class TestPrintModifiedAPI:
    """Tests for the /server/helix/print_modified endpoint (path-based API)."""

    @pytest.mark.asyncio
    async def test_rejects_missing_original(self, helix_print_component, mock_server,
                                            temp_gcodes_dir):
        """Test API rejects request when original file doesn't exist."""
        # Initialize component
        await helix_print_component.component_init()

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "nonexistent.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        with pytest.raises(Exception) as exc_info:
            await handler(request)

        assert "not found" in str(exc_info.value).lower()

    @pytest.mark.asyncio
    async def test_uses_uploaded_temp_file(self, helix_print_component, mock_server,
                                           temp_gcodes_dir):
        """Test API uses the pre-uploaded temp file."""
        # Create original file
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\nBED_MESH_CALIBRATE\nG1 X0 Y0\n")

        # Create temp file with modified content (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n; BED_MESH_CALIBRATE disabled\nG1 X0 Y0\n")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": ["bed_leveling_disabled"],
        })

        result = await handler(request)

        assert result["original_filename"] == "benchy.gcode"
        assert result["status"] == "printing"
        assert result["temp_filename"] == ".helix_temp/mod_benchy.gcode"

    @pytest.mark.asyncio
    async def test_creates_symlink(self, helix_print_component, mock_server,
                                   temp_gcodes_dir):
        """Test API creates symlink to temp file."""
        # Create original file
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\n")

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        result = await handler(request)

        # Verify symlink was created
        symlink_path = Path(temp_gcodes_dir) / result["print_filename"]
        assert symlink_path.is_symlink()

    @pytest.mark.asyncio
    async def test_starts_print_with_symlink(self, helix_print_component, mock_server,
                                             temp_gcodes_dir):
        """Test API starts print using symlink path."""
        # Create original file
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\n")

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        await handler(request)

        # Verify print command was sent via klippy_apis (NOT klippy_connection,
        # which has no run_gcode method - see MockKlippy/MockKlippyApis docs)
        klippy_apis = mock_server.components["klippy_apis"]
        assert klippy_apis.run_gcode.await_count == 1
        sent_command = klippy_apis.run_gcode.await_args.args[0]
        assert ".helix_print/full/benchy.gcode" in sent_command

    @pytest.mark.asyncio
    async def test_print_start_calls_klippy_apis_not_klippy_connection(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        """Regression test for production crash (debug bundle RA6EPJTZ):

        'Failed to start print: 'KlippyConnection' object has no attribute
        run_gcode''. Starting a print must call klippy_apis.run_gcode exactly
        once with an SDCARD_PRINT_FILE command naming the symlink - not
        klippy_connection, which has no run_gcode method in real Moonraker.

        This test FAILS against the old (klippy_connection.run_gcode) code and
        PASSES after routing through klippy_apis.
        """
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\n")

        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        result = await handler(request)
        assert result["status"] == "printing"

        klippy_apis = mock_server.components["klippy_apis"]
        assert klippy_apis.run_gcode.await_count == 1
        sent_command = klippy_apis.run_gcode.await_args.args[0]
        assert "SDCARD_PRINT_FILE" in sent_command
        assert 'FILENAME=".helix_print/full/benchy.gcode"' in sent_command

        # klippy_connection must never be touched for this - it has no
        # run_gcode method on real Moonraker.
        klippy_connection = mock_server.components["klippy_connection"]
        assert not hasattr(klippy_connection, "run_gcode")

    @pytest.mark.asyncio
    async def test_disabled_returns_error(self, mock_server, temp_gcodes_dir):
        """Test API returns error when component is disabled."""
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = MockDatabase()

        config = MockConfigHelper(mock_server, {"enabled": False})
        component = load_component(config)

        # Create temp file
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_test.gcode"
        temp_file.write_text("G28\n")

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "test.gcode",
            "temp_file_path": ".helix_temp/mod_test.gcode",
        })

        with pytest.raises(Exception) as exc_info:
            await handler(request)

        assert "disabled" in str(exc_info.value).lower()


# ============================================================================
# Symlink Conflict Tests
# ============================================================================

class TestPrintModifiedToolsMap:
    """#2038: a print the plugin starts carries the app's tool map to a
    Moonraker whose start_print takes one (Z-Mod's print_start_tools)."""

    @staticmethod
    def _stage(temp_gcodes_dir):
        (Path(temp_gcodes_dir) / "benchy.gcode").write_text("G28\n")
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        (temp_dir / "mod_benchy.gcode").write_text("G28\n")

    @staticmethod
    def _request(**extra):
        params = {
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        }
        params.update(extra)
        return MockWebRequest(params)

    @staticmethod
    def _tools_start_print(apis):
        calls = []

        async def start_print(filename, wait_klippy_started=False, user=None, tools=None):
            calls.append((filename, tools))

        apis.start_print = start_print
        return calls

    @pytest.mark.asyncio
    async def test_tools_go_through_start_print_when_it_takes_them(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        self._stage(temp_gcodes_dir)
        await helix_print_component.component_init()
        apis = mock_server.components["klippy_apis"]
        calls = self._tools_start_print(apis)

        handler = mock_server.endpoints["/server/helix/print_modified"]
        result = await handler(self._request(tools="2,,1"))

        assert result["status"] == "printing"
        assert calls == [(".helix_print/full/benchy.gcode", "2,,1")]
        assert apis.run_gcode.await_count == 0

    @pytest.mark.asyncio
    async def test_stock_start_print_starts_without_the_map(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        self._stage(temp_gcodes_dir)
        await helix_print_component.component_init()
        apis = mock_server.components["klippy_apis"]

        async def start_print(filename, wait_klippy_started=False, user=None):
            raise AssertionError("stock start_print must not be called with tools")

        apis.start_print = start_print

        handler = mock_server.endpoints["/server/helix/print_modified"]
        await handler(self._request(tools="2,1"))

        assert apis.run_gcode.await_count == 1
        sent = apis.run_gcode.await_args.args[0]
        assert sent == 'SDCARD_PRINT_FILE FILENAME=".helix_print/full/benchy.gcode"'

    @pytest.mark.asyncio
    async def test_no_map_keeps_the_plain_start(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        self._stage(temp_gcodes_dir)
        await helix_print_component.component_init()
        apis = mock_server.components["klippy_apis"]
        calls = self._tools_start_print(apis)

        handler = mock_server.endpoints["/server/helix/print_modified"]
        await handler(self._request())

        assert calls == []
        assert apis.run_gcode.await_count == 1

    @pytest.mark.asyncio
    @pytest.mark.parametrize("tools", ["2;M112", "T0=2", "1, 2", "x" * 600])
    async def test_a_malformed_map_starts_nothing(
        self, helix_print_component, mock_server, temp_gcodes_dir, tools
    ):
        self._stage(temp_gcodes_dir)
        await helix_print_component.component_init()
        apis = mock_server.components["klippy_apis"]
        calls = self._tools_start_print(apis)

        handler = mock_server.endpoints["/server/helix/print_modified"]
        with pytest.raises(Exception, match="400"):
            await handler(self._request(tools=tools))

        assert calls == []
        assert apis.run_gcode.await_count == 0
        assert (Path(temp_gcodes_dir) / ".helix_temp" / "mod_benchy.gcode").exists()


class TestSymlinkConflicts:
    """Tests for symlink conflict handling."""

    @pytest.mark.asyncio
    async def test_replaces_existing_symlink(self, helix_print_component, mock_server,
                                             temp_gcodes_dir):
        """Test that existing symlinks are replaced."""
        # Create original file
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\n")

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        # Create existing symlink
        symlink_dir = Path(temp_gcodes_dir) / ".helix_print"
        symlink_dir.mkdir(parents=True, exist_ok=True)
        existing_symlink = symlink_dir / "full" / "benchy.gcode"
        existing_symlink.parent.mkdir(parents=True, exist_ok=True)
        existing_symlink.symlink_to("/nonexistent")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        # Should succeed, replacing the existing symlink
        result = await handler(request)
        assert result["status"] == "printing"


# ============================================================================
# Active Print Tracking Tests
# ============================================================================

class TestActivePrintTracking:
    """Tests for active print tracking."""

    @pytest.mark.asyncio
    async def test_tracks_active_print(self, helix_print_component, mock_server,
                                       temp_gcodes_dir):
        """Test that active prints are tracked."""
        # Create original file
        original = Path(temp_gcodes_dir) / "benchy.gcode"
        original.write_text("G28\n")

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": ["test_mod"],
        })

        result = await handler(request)

        # Check active prints
        assert len(helix_print_component.active_prints) == 1
        print_info = helix_print_component.active_prints[result["print_filename"]]
        assert print_info.original_filename == "benchy.gcode"
        assert print_info.modifications == ["test_mod"]


# ============================================================================
# Path Validation Tests
# ============================================================================

class TestPathValidation:
    """Tests for path validation and security."""

    @pytest.mark.asyncio
    async def test_handles_subdirectory_path(self, helix_print_component, mock_server,
                                             temp_gcodes_dir):
        """Test handling of files in subdirectories."""
        # Create subdirectory and file
        subdir = Path(temp_gcodes_dir) / "prints" / "2024"
        subdir.mkdir(parents=True, exist_ok=True)
        original = subdir / "benchy.gcode"
        original.write_text("G28\n")

        # Create temp file (simulating client upload)
        temp_dir = Path(temp_gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        temp_file = temp_dir / "mod_benchy.gcode"
        temp_file.write_text("G28\n")

        # Initialize component
        await helix_print_component.component_init()

        handler = mock_server.endpoints["/server/helix/print_modified"]
        request = MockWebRequest({
            "original_filename": "prints/2024/benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

        result = await handler(request)
        assert result["status"] == "printing"

    @pytest.mark.asyncio
    @pytest.mark.parametrize("temp", [
        "benchy.gcode",
        "other.gcode",
        "prints/mod_benchy.gcode",
        ".helix_temp/nested/mod_benchy.gcode",
        ".helix_print/mod_benchy.gcode",
    ])
    async def test_the_copy_must_sit_in_a_staging_dir(
        self, helix_print_component, mock_server, temp_gcodes_dir, temp
    ):
        """A failed start deletes the copy, so a copy path naming the original
        or another user file would delete that file."""
        gcodes = Path(temp_gcodes_dir)
        for name in ("benchy.gcode", "other.gcode", "prints/mod_benchy.gcode",
                     ".helix_temp/nested/mod_benchy.gcode", ".helix_print/mod_benchy.gcode"):
            (gcodes / name).parent.mkdir(parents=True, exist_ok=True)
            (gcodes / name).write_text("G28\n")
        await helix_print_component.component_init()
        apis = mock_server.components["klippy_apis"]
        apis.run_gcode.side_effect = RuntimeError("klippy down")

        with pytest.raises(Exception, match="400"):
            await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
                "original_filename": "benchy.gcode", "temp_file_path": temp,
                "modifications": [],
            }))
        assert (gcodes / temp).read_text() == "G28\n"
        apis.run_gcode.assert_not_awaited()

    @pytest.mark.asyncio
    async def test_a_staged_link_to_the_original_is_refused(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        (gcodes / "benchy.gcode").write_text("G28\n")
        (gcodes / ".helix_temp").mkdir()
        (gcodes / ".helix_temp" / "mod_benchy.gcode").symlink_to(gcodes / "benchy.gcode")
        await helix_print_component.component_init()
        with pytest.raises(Exception, match="400"):
            await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
                "original_filename": "benchy.gcode",
                "temp_file_path": ".helix_temp/mod_benchy.gcode", "modifications": [],
            }))
        assert (gcodes / "benchy.gcode").read_text() == "G28\n"

    @pytest.mark.asyncio
    async def test_a_staged_original_is_never_its_own_copy(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        (gcodes / ".helix_temp").mkdir()
        (gcodes / ".helix_temp" / "mod_benchy.gcode").write_text("G28\n")
        await helix_print_component.component_init()
        mock_server.components["klippy_apis"].run_gcode.side_effect = RuntimeError("down")
        with pytest.raises(Exception, match="400.*original"):
            await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
                "original_filename": ".helix_temp/mod_benchy.gcode",
                "temp_file_path": ".helix_temp/mod_benchy.gcode", "modifications": [],
            }))
        assert (gcodes / ".helix_temp" / "mod_benchy.gcode").read_text() == "G28\n"

    @pytest.mark.asyncio
    async def test_the_visible_staging_dir_is_accepted(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        """Where the link dir is visible, the app stages under helix_staging
        whatever temp_dir says."""
        gcodes = Path(temp_gcodes_dir)
        (gcodes / "benchy.gcode").write_text("G28\n")
        (gcodes / "helix_staging").mkdir()
        (gcodes / "helix_staging" / "modified_1_benchy.gcode").write_text("G28\n")
        await helix_print_component.component_init()
        result = await mock_server.endpoints["/server/helix/print_modified"](MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": "helix_staging/modified_1_benchy.gcode", "modifications": [],
        }))
        assert result["status"] == "printing"

    @staticmethod
    def _stage_nested(gcodes_dir):
        subdir = Path(gcodes_dir) / "prints" / "2024"
        subdir.mkdir(parents=True, exist_ok=True)
        (subdir / "benchy.gcode").write_text("G28\n")
        temp_dir = Path(gcodes_dir) / ".helix_temp"
        temp_dir.mkdir(parents=True, exist_ok=True)
        (temp_dir / "mod_benchy.gcode").write_text("G28\n")
        return MockWebRequest({
            "original_filename": "prints/2024/benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

    @pytest.mark.asyncio
    async def test_symlink_mirrors_the_original_full_path(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        """The printed filename names the original's whole path, so a client
        recovers it from print_stats alone."""
        request = self._stage_nested(temp_gcodes_dir)
        await helix_print_component.component_init()

        result = await mock_server.endpoints["/server/helix/print_modified"](request)

        assert result["print_filename"] == ".helix_print/full/prints/2024/benchy.gcode"
        assert (Path(temp_gcodes_dir) / result["print_filename"]).is_symlink()

    @pytest.mark.asyncio
    @pytest.mark.parametrize("original,link", [
        ("prints/benchy.gcode.3mf", ".helix_print/full/prints/benchy.gcode"),
        ("prints/benchy.3MF", ".helix_print/full/prints/benchy.gcode"),
    ])
    async def test_a_3mf_original_links_under_a_gcode_name(
        self, helix_print_component, mock_server, temp_gcodes_dir, original, link
    ):
        """Klipper's virtual_sdcard opens only .gcode/.g/.gco names, so the
        link for a .3mf original carries the .gcode name it stands for, while
        the original keeps its own name for history."""
        gcodes = Path(temp_gcodes_dir)
        (gcodes / "prints").mkdir()
        (gcodes / original).write_bytes(b"PK\x03\x04")
        (gcodes / ".helix_temp").mkdir()
        (gcodes / ".helix_temp" / "plate.gcode").write_text("G28\n")
        await helix_print_component.component_init()

        result = await mock_server.endpoints["/server/helix/print_modified"](
            MockWebRequest({
                "original_filename": original,
                "temp_file_path": ".helix_temp/plate.gcode",
                "modifications": [],
            })
        )

        assert result["print_filename"] == link
        assert result["original_filename"] == original
        assert (gcodes / link).is_symlink()
        apis = mock_server.components["klippy_apis"]
        apis.run_gcode.assert_awaited_with(f'SDCARD_PRINT_FILE FILENAME="{link}"')

    @pytest.mark.asyncio
    async def test_rejects_a_symlink_path_that_escapes(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        """A directory under the symlink dir that links back into the user's
        files cannot carry the print symlink there, where it would replace the
        original itself."""
        request = self._stage_nested(temp_gcodes_dir)
        await helix_print_component.component_init()
        user_dir = Path(temp_gcodes_dir) / "prints"
        full_dir = Path(temp_gcodes_dir) / ".helix_print" / "full"
        full_dir.mkdir(parents=True, exist_ok=True)
        (full_dir / "prints").symlink_to(user_dir)

        with pytest.raises(Exception) as exc_info:
            await mock_server.endpoints["/server/helix/print_modified"](request)

        assert "traversal" in str(exc_info.value).lower()
        original = user_dir / "2024" / "benchy.gcode"
        assert not original.is_symlink()
        assert original.read_text() == "G28\n"

    @pytest.mark.asyncio
    async def test_cleanup_removes_the_empty_directories(
        self, helix_print_component, mock_server, temp_gcodes_dir
    ):
        request = self._stage_nested(temp_gcodes_dir)
        await helix_print_component.component_init()
        result = await mock_server.endpoints["/server/helix/print_modified"](request)
        symlink_root = Path(temp_gcodes_dir) / ".helix_print"
        full_dir = symlink_root / "full"
        # A sibling keeps its directory; only what empties goes.
        (full_dir / "prints" / "keep.gcode").write_text("")

        info = helix_print_component.active_prints[result["print_filename"]]
        await helix_print_component._schedule_cleanup(info)

        assert not (full_dir / "prints" / "2024").exists()
        assert (full_dir / "prints").is_dir()
        assert symlink_root.is_dir()

        (full_dir / "prints" / "keep.gcode").unlink()
        helix_print_component._remove_symlink(full_dir / "prints" / "gone.gcode")
        assert not (full_dir / "prints").exists()
        assert symlink_root.is_dir()

    @pytest.mark.asyncio
    async def test_failed_link_leaves_no_directories(
        self, helix_print_component, mock_server, temp_gcodes_dir, monkeypatch
    ):
        request = self._stage_nested(temp_gcodes_dir)
        await helix_print_component.component_init()

        def refuse(symlink_path, target_path):
            raise OSError("link refused")

        monkeypatch.setattr(helix_print_component, "_create_symlink_atomic", refuse)
        with pytest.raises(Exception):
            await mock_server.endpoints["/server/helix/print_modified"](request)

        symlink_root = Path(temp_gcodes_dir) / ".helix_print"
        assert symlink_root.is_dir()
        assert not (symlink_root / "full").exists()


# ============================================================================
# History Patching Tests
# ============================================================================

def _print_info(job_id="00001A", modifications=None):
    info = PrintInfo(
        original_filename="prints/benchy.gcode",
        temp_filename=".helix_temp/mod_benchy.gcode",
        symlink_filename=".helix_print/abcd_benchy.gcode",
        modifications=modifications if modifications is not None else ["pa_tuning"],
        start_time=100.0,
    )
    info.job_id = job_id
    return info


def _helix_entry(job):
    return [e for e in job.auxiliary_data if e.get("provider") == "helix_print"]


class TestHistoryPatching:
    """Tests for rewriting the history entry after a modified print."""

    @pytest.mark.asyncio
    async def test_rewrites_filename_to_original(self, helix_print_component,
                                                 mock_server):
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        assert len(history.save_job_calls) == 1
        job, job_id = history.save_job_calls[0]
        assert job.filename == "prints/benchy.gcode"
        assert job_id == 0x1A
        # Columns that are not ours survive the round trip
        assert job.status == "completed"
        assert job.total_duration == 100.0

    @pytest.mark.asyncio
    async def test_strips_symlink_dir_prefix(self, helix_print_component,
                                             mock_server):
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()

        info = _print_info()
        info.original_filename = ".helix_print/benchy.gcode"
        await helix_print_component._patch_history_entry(info, "complete")

        job, _ = history.save_job_calls[0]
        assert job.filename == "benchy.gcode"

    @pytest.mark.asyncio
    async def test_appends_to_existing_auxiliary_data(self, helix_print_component,
                                                      mock_server):
        history = mock_server.components["history"]
        other = {"provider": "spoolman", "name": "spool_id", "value": 7}
        history.add_job("00001A", auxiliary_data=[other])
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        job, _ = history.save_job_calls[0]
        assert isinstance(job.auxiliary_data, list)
        assert other in job.auxiliary_data
        ours = _helix_entry(job)
        assert len(ours) == 1
        assert ours[0]["value"]["original"] == "prints/benchy.gcode"
        assert ours[0]["value"]["modifications"] == ["pa_tuning"]
        assert ours[0]["value"]["temp_file"] == ".helix_temp/mod_benchy.gcode"

    @pytest.mark.asyncio
    async def test_repatch_replaces_own_entry(self, helix_print_component,
                                              mock_server):
        history = mock_server.components["history"]
        stale = {"provider": "helix_print", "name": "modifications", "value": {}}
        history.add_job("00001A", auxiliary_data=[stale])
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        job, _ = history.save_job_calls[0]
        ours = _helix_entry(job)
        assert len(ours) == 1
        assert ours[0]["value"]["modifications"] == ["pa_tuning"]

    @pytest.mark.asyncio
    async def test_hex_string_job_id(self, helix_print_component, mock_server):
        history = mock_server.components["history"]
        history.add_job("00FF01")
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(
            _print_info(job_id="00FF01"), "complete"
        )

        job, job_id = history.save_job_calls[0]
        assert job_id == 0xFF01
        assert job.filename == "prints/benchy.gcode"

    @pytest.mark.asyncio
    async def test_int_job_id(self, helix_print_component, mock_server):
        history = mock_server.components["history"]
        history.add_job(26)
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(
            _print_info(job_id=26), "complete"
        )

        _, job_id = history.save_job_calls[0]
        assert job_id == 26

    @pytest.mark.asyncio
    async def test_missing_job_is_quiet(self, helix_print_component, mock_server):
        history = mock_server.components["history"]
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        assert history.save_job_calls == []

    @pytest.mark.asyncio
    async def test_no_job_id_skips_lookup(self, helix_print_component, mock_server):
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()

        info = _print_info()
        info.job_id = None
        await helix_print_component._patch_history_entry(info, "complete")

        assert history.save_job_calls == []

    @pytest.mark.asyncio
    async def test_history_without_save_job_warns(self, mock_server,
                                                  temp_gcodes_dir, caplog):
        class AncientHistory:
            async def get_job(self, job_id):
                return {"job_id": job_id, "filename": "x.gcode"}

        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = MockDatabase()
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = AncientHistory()
        component = load_component(MockConfigHelper(mock_server, {
            "temp_dir": ".helix_temp",
            "symlink_dir": ".helix_print",
            "cleanup_delay": 3600,
            "enabled": True,
        }))
        await component.component_init()

        await component._patch_history_entry(_print_info(), "complete")

        assert "History rewrite unavailable" in caplog.text

    @pytest.mark.asyncio
    async def test_missing_printer_job_class_warns(self, helix_print_component,
                                                   mock_server, caplog,
                                                   monkeypatch):
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()
        monkeypatch.delattr(sys.modules[MockHistory.__module__], "PrinterJob")

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        assert history.save_job_calls == []
        assert "no PrinterJob" in caplog.text

    @pytest.mark.asyncio
    async def test_failing_history_does_not_raise(self, helix_print_component,
                                                  mock_server):
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()

        async def boom(job, job_id=None):
            raise RuntimeError("database is locked")

        history.save_job = boom
        await helix_print_component._patch_history_entry(_print_info(), "complete")



    @pytest.mark.asyncio
    async def test_dict_shaped_auxiliary_data_is_dropped_not_walked(
        self, helix_print_component, mock_server
    ):
        # auxiliary_data is a list from Moonraker v0.9.0, the same release that
        # added save_job. A fork handing back the older mapping would otherwise
        # be iterated by element - yielding its KEYS - and written back as a
        # list of strings over the real thing.
        history = mock_server.components["history"]
        history.add_job("00001A", auxiliary_data={"spoolman_id": 7, "other": 1})
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        job, _ = history.save_job_calls[0]
        assert isinstance(job.auxiliary_data, list)
        assert job.auxiliary_data == _helix_entry(job)
        assert "spoolman_id" not in job.auxiliary_data

    @pytest.mark.asyncio
    async def test_a_list_of_other_providers_is_kept(
        self, helix_print_component, mock_server
    ):
        history = mock_server.components["history"]
        spoolman = {"provider": "spoolman", "name": "spool_id", "value": 7}
        history.add_job("00001A", auxiliary_data=[spoolman])
        await helix_print_component.component_init()

        await helix_print_component._patch_history_entry(_print_info(), "complete")

        job, _ = history.save_job_calls[0]
        assert spoolman in job.auxiliary_data
        assert len(_helix_entry(job)) == 1


if __name__ == "__main__":
    pytest.main([__file__, "-v"])

class TestHistoryIdCapture:
    """The history id arrives on history's own event, not in print_stats."""

    def _payload(self, action="added", filename=".helix_print/abcd_benchy.gcode",
                 job_id="00001A"):
        job = {"filename": filename, "status": "in_progress"}
        if job_id is not None:
            job["job_id"] = job_id
        return {"action": action, "job": job}

    def _staged(self, component, filename=".helix_print/abcd_benchy.gcode"):
        info = PrintInfo(
            original_filename="prints/benchy.gcode",
            temp_filename=".helix_temp/modified_1_benchy.gcode",
            symlink_filename=filename,
            modifications=["remap_T1_to_T2"],
            start_time=100.0,
        )
        component.active_prints[filename] = info
        return info

    @pytest.mark.asyncio
    async def test_added_event_records_the_id(self, helix_print_component):
        info = self._staged(helix_print_component)
        assert info.job_id is None

        await helix_print_component._on_history_changed(self._payload())

        assert info.job_id == "00001A"

    @pytest.mark.asyncio
    async def test_klipper_print_stats_never_carry_an_id(self, helix_print_component):
        # Klipper's print_stats has no job_id, so the job-state route is not
        # where the id comes from. Planting one here and watching it be ignored
        # is what makes that a claim about our code rather than about the
        # fixture happening to leave the key out.
        info = self._staged(helix_print_component)

        await helix_print_component._on_job_state_changed(
            None,
            {"state": "standby", "filename": ".helix_print/abcd_benchy.gcode"},
            {
                "state": "printing",
                "filename": ".helix_print/abcd_benchy.gcode",
                "job_id": "00BEEF",
            },
        )

        assert info.job_id is None

    @pytest.mark.asyncio
    async def test_other_peoples_prints_are_ignored(self, helix_print_component):
        info = self._staged(helix_print_component)

        await helix_print_component._on_history_changed(
            self._payload(filename="prints/somebody_elses.gcode")
        )

        assert info.job_id is None

    @pytest.mark.asyncio
    async def test_finished_action_does_not_record(self, helix_print_component):
        # Only the "added" action carries an id for a job we are still staging.
        info = self._staged(helix_print_component)

        await helix_print_component._on_history_changed(self._payload(action="finished"))

        assert info.job_id is None

    @pytest.mark.asyncio
    async def test_payload_without_an_id_is_ignored(self, helix_print_component):
        info = self._staged(helix_print_component)

        await helix_print_component._on_history_changed(self._payload(job_id=None))

        assert info.job_id is None

    @pytest.mark.asyncio
    async def test_the_handler_is_registered(self, helix_print_component, mock_server):
        await helix_print_component.component_init()
        assert "history:history_changed" in mock_server.event_handlers

    @pytest.mark.asyncio
    async def test_id_from_the_event_reaches_the_patch(self, helix_print_component,
                                                       mock_server):
        # The whole chain: history announces the row, we keep the id, and the
        # patch uses it to address that row.
        history = mock_server.components["history"]
        history.add_job("00001A")
        await helix_print_component.component_init()
        info = self._staged(helix_print_component)

        await helix_print_component._on_history_changed(self._payload())
        await helix_print_component._patch_history_entry(info, "complete")

        assert len(history.save_job_calls) == 1
        job, job_id = history.save_job_calls[0]
        assert job_id == 0x1A
        assert job.filename == "prints/benchy.gcode"


# ============================================================================
# Visible symlink directory
# ============================================================================

class TestVisibleSymlinkDir:
    """symlink_dir without a leading dot, as the installer sets it where
    Klipper's virtual_sdcard will not open a file under a hidden directory."""

    @pytest.fixture
    def component(self, mock_server, temp_gcodes_dir):
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = MockDatabase()
        mock_server.components["klippy_connection"] = MockKlippy()
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = MockHistory()
        return load_component(MockConfigHelper(mock_server, {
            "temp_dir": ".helix_temp",
            "symlink_dir": "helix_print",
            "cleanup_delay": 3600,
            "enabled": True,
        }))

    @staticmethod
    def _stage(gcodes: Path) -> MockWebRequest:
        (gcodes / "parts").mkdir()
        (gcodes / "parts" / "benchy.gcode").write_text("G28\n")
        (gcodes / ".helix_temp").mkdir(exist_ok=True)
        (gcodes / ".helix_temp" / "mod_benchy.gcode").write_text("G28\nG1 X1\n")
        return MockWebRequest({
            "original_filename": "parts/benchy.gcode",
            "temp_file_path": ".helix_temp/mod_benchy.gcode",
            "modifications": [],
        })

    @pytest.mark.asyncio
    async def test_print_links_under_the_visible_directory(
        self, component, mock_server, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        request = self._stage(gcodes)
        await component.component_init()
        assert (gcodes / "helix_print").is_dir()

        result = await mock_server.endpoints["/server/helix/print_modified"](request)

        link = "helix_print/full/parts/benchy.gcode"
        assert result["print_filename"] == link
        assert (gcodes / link).is_symlink()
        assert (gcodes / link).read_text() == "G28\nG1 X1\n"
        mock_server.components["klippy_apis"].run_gcode.assert_awaited_with(
            f'SDCARD_PRINT_FILE FILENAME="{link}"')

    @pytest.mark.asyncio
    async def test_finished_print_is_cleaned_up_and_attributed(
        self, component, mock_server, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        request = self._stage(gcodes)
        await component.component_init()
        result = await mock_server.endpoints["/server/helix/print_modified"](request)
        link = result["print_filename"]

        await component._on_job_state_changed(
            None, {"state": "printing", "filename": link},
            {"state": "complete", "filename": link})

        assert link not in component.active_prints
        assert not (gcodes / link).is_symlink()
        assert not (gcodes / "helix_print" / "full").exists()
        assert (gcodes / "helix_print").is_dir()

    @pytest.mark.asyncio
    async def test_startup_removes_links_whose_file_is_gone(
        self, component, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        await component.component_init()
        full = gcodes / "helix_print" / "full"
        (full / "a").mkdir(parents=True)
        (full / "keep").mkdir()
        live_target = gcodes / ".helix_temp" / "live.gcode"
        live_target.write_text("G28\n")
        (full / "a" / "stale.gcode").symlink_to(gcodes / ".helix_temp" / "gone.gcode")
        (full / "keep" / "live.gcode").symlink_to(live_target)

        await component._startup_cleanup()

        assert not (full / "a").exists()
        assert (full / "keep" / "live.gcode").is_symlink()
        assert (gcodes / "helix_print").is_dir()

    @pytest.mark.asyncio
    async def test_the_sweep_never_follows_a_linked_directory_out(
        self, component, temp_gcodes_dir
    ):
        gcodes = Path(temp_gcodes_dir)
        await component.component_init()
        outside = gcodes / "user"
        outside.mkdir()
        (outside / "dangling.gcode").symlink_to(gcodes / "nowhere.gcode")
        (gcodes / "helix_print" / "full").mkdir(parents=True)
        (gcodes / "helix_print" / "full" / "user").symlink_to(outside)

        await component._startup_cleanup()

        assert (outside / "dangling.gcode").is_symlink()


# ============================================================================
# Namespace persistence on Moonraker 0.9.3's database
# ============================================================================

class Moonraker093Database:
    """The namespace half of Moonraker v0.9.3's `database` component, as
    vendored by Forge-X: no execute_db_command (so the plugin takes the
    namespace route), no ns_items, string keys split on "." with an empty
    segment refused, nested inserts for multi-segment keys, and get_item
    with no key returning the whole namespace."""

    def __init__(self):
        self.ns: Dict[str, Dict[str, Any]] = {}

    class Error(Exception):
        def __init__(self, message, status_code=400):
            super().__init__(message)
            self.status_code = status_code

    @classmethod
    def _parse(cls, key):
        keys = key if isinstance(key, list) else key.split(".")
        if not keys or "" in keys:
            raise cls.Error(f"Invalid Key Format: '{key}'")
        return keys

    async def insert_item(self, namespace, key, value):
        keys = self._parse(key)
        store = self.ns.setdefault(namespace, {})
        if len(keys) == 1:
            store[keys[0]] = value
            return
        record = store.get(keys[0])
        if not isinstance(record, dict):
            record = {}
        item = record
        for k in keys[1:-1]:
            item = item.setdefault(k, {})
        item[keys[-1]] = value
        store[keys[0]] = record

    async def update_item(self, namespace, key, value):
        keys = self._parse(key)
        store = self.ns.get(namespace, {})
        if keys[0] not in store:
            raise self.Error(f"Key '{key}' in namespace '{namespace}' not found", 404)
        if len(keys) == 1 and isinstance(value, dict) and isinstance(store[keys[0]], dict):
            store[keys[0]].update(value)
        else:
            await self.insert_item(namespace, key, value)

    async def get_item(self, namespace, key=None, default=Exception):
        try:
            if key is None:
                if namespace not in self.ns:
                    raise self.Error(f"Namespace {namespace} not found", 404)
                return dict(self.ns[namespace])
            keys = self._parse(key)
            value = self.ns[namespace][keys[0]]
            for k in keys[1:]:
                value = value[k]
            return value
        except (KeyError, TypeError, self.Error):
            if default is not Exception:
                return default
            raise self.Error(f"Key '{key}' in namespace '{namespace}' not found", 404)

    async def delete_item(self, namespace, key):
        keys = self._parse(key)
        return self.ns.get(namespace, {}).pop(keys[0], None)


class TestNamespacePersistence093:
    @pytest.fixture
    def db(self):
        return Moonraker093Database()

    @pytest.fixture
    def component(self, mock_server, temp_gcodes_dir, db):
        mock_server.components["file_manager"] = MockFileManager(temp_gcodes_dir)
        mock_server.components["database"] = db
        mock_server.components["klippy_connection"] = MockKlippy()
        mock_server.components["klippy_apis"] = MockKlippyApis()
        mock_server.components["history"] = MockHistory()
        return load_component(MockConfigHelper(mock_server, {
            "temp_dir": ".helix_temp", "symlink_dir": ".helix_print",
            "cleanup_delay": 3600, "enabled": True,
        }))

    @staticmethod
    def _stage(gcodes: Path) -> MockWebRequest:
        (gcodes / "benchy.gcode").write_text("G28\n")
        (gcodes / ".helix_temp").mkdir(exist_ok=True)
        (gcodes / ".helix_temp" / "modified_1_benchy.gcode").write_text("G28\nG1\n")
        return MockWebRequest({
            "original_filename": "benchy.gcode",
            "temp_file_path": ".helix_temp/modified_1_benchy.gcode",
            "modifications": [],
        })

    @pytest.mark.asyncio
    async def test_takes_the_namespace_route(self, component):
        await component.component_init()
        assert component._use_namespace and not component._use_sqlite

    @pytest.mark.asyncio
    async def test_a_dotted_temp_name_stores_as_one_record(
        self, component, mock_server, temp_gcodes_dir, db
    ):
        await component.component_init()
        await mock_server.endpoints["/server/helix/print_modified"](
            self._stage(Path(temp_gcodes_dir)))

        record = db.ns[HELIX_NAMESPACE][".helix_temp/modified_1_benchy.gcode"]
        assert record["status"] == "active"
        assert record["symlink_filename"] == ".helix_print/full/benchy.gcode"

    @pytest.mark.asyncio
    async def test_an_overdue_record_is_cleaned_at_startup(
        self, component, mock_server, temp_gcodes_dir, db
    ):
        gcodes = Path(temp_gcodes_dir)
        await component.component_init()
        result = await mock_server.endpoints["/server/helix/print_modified"](
            self._stage(gcodes))
        info = component.active_prints[result["print_filename"]]
        await component._schedule_cleanup(info)
        record = db.ns[HELIX_NAMESPACE][".helix_temp/modified_1_benchy.gcode"]
        assert record["status"] == "pending_cleanup"
        record["cleanup_scheduled_at"] = time.time() - 1

        await component._startup_cleanup()

        assert not (gcodes / ".helix_temp" / "modified_1_benchy.gcode").exists()
        assert db.ns[HELIX_NAMESPACE][".helix_temp/modified_1_benchy.gcode"]["status"] == "cleaned"

    @pytest.mark.asyncio
    async def test_old_cleaned_records_are_purged(self, component, db):
        await component.component_init()
        old = time.time() - DB_RECORD_MAX_AGE - 10
        await db.insert_item(HELIX_NAMESPACE, [".helix_temp/a.gcode"], {
            "temp_filename": ".helix_temp/a.gcode", "symlink_filename": "x",
            "status": "cleaned", "created_at": old})
        await db.insert_item(HELIX_NAMESPACE, [".helix_temp/b.gcode"], {
            "temp_filename": ".helix_temp/b.gcode", "symlink_filename": "y",
            "status": "cleaned", "created_at": time.time()})

        await component._startup_cleanup()

        assert list(db.ns[HELIX_NAMESPACE]) == [".helix_temp/b.gcode"]

    @pytest.mark.asyncio
    async def test_no_namespace_yet_starts_cleanly(self, component, db):
        await component.component_init()
        await component._startup_cleanup()
        assert HELIX_NAMESPACE not in db.ns

    @pytest.mark.asyncio
    async def test_a_record_left_nested_by_a_string_key_is_rekeyed_and_cleaned(
        self, component, temp_gcodes_dir, db
    ):
        """A temp dir without a leading dot let a string key store: split on
        its dots, the record sits nested under the first segment."""
        gcodes = Path(temp_gcodes_dir)
        (gcodes / "helix_temp").mkdir()
        (gcodes / "helix_temp" / "modified_1_benchy.gcode").write_text("G28\n")
        record = {
            "temp_filename": "helix_temp/modified_1_benchy.gcode",
            "symlink_filename": ".helix_print/full/benchy.gcode",
            "status": "pending_cleanup", "cleanup_scheduled_at": time.time() - 1,
            "created_at": time.time() - 100,
        }
        await db.insert_item(HELIX_NAMESPACE, "helix_temp/modified_1_benchy.gcode", record)
        assert "helix_temp/modified_1_benchy" in db.ns[HELIX_NAMESPACE]
        await component.component_init()

        await component._startup_cleanup()

        store = db.ns[HELIX_NAMESPACE]
        assert "helix_temp/modified_1_benchy" not in store
        assert store["helix_temp/modified_1_benchy.gcode"]["status"] == "cleaned"
        assert not (gcodes / "helix_temp" / "modified_1_benchy.gcode").exists()

    @pytest.mark.asyncio
    async def test_unrelated_values_in_the_namespace_do_not_break_startup(
        self, component, db
    ):
        await db.insert_item(HELIX_NAMESPACE, ["junk"], "not a record")
        await db.insert_item(HELIX_NAMESPACE, ["nested"], {"a": {"b": 1}})
        await component.component_init()

        await component._startup_cleanup()

        assert db.ns[HELIX_NAMESPACE]["junk"] == "not a record"
        assert db.ns[HELIX_NAMESPACE]["nested"] == {"a": {"b": 1}}
