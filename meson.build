project(
	'augur',
	'c',
	version: '0.1.1',
	license: 'BSD-3-Clause',
	meson_version: '>=1.0.0',
	default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'],
)

add_project_arguments(
	'-DAUGUR_VERSION="@0@"'.format(meson.project_version()),
	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
	language: 'c',
)

glib = dependency('glib-2.0', version: '>=2.74')
gio = dependency('gio-2.0')
gio_unix = dependency('gio-unix-2.0')
json = dependency('json-glib-1.0')
soup = dependency('libsoup-3.0')
libm = meson.get_compiler('c').find_library('m', required: false)

augurd = executable(
	'augurd',
	['src/augurd.c', 'src/classify.c', 'src/config.c', 'src/provider.c',
		'src/schema.c'],
	dependencies: [glib, gio, gio_unix, json, soup, libm],
	install: true,
	install_dir: get_option('libexecdir'),
)

augur = executable(
	'augur',
	'src/augur.c',
	dependencies: [glib, gio, gio_unix, json],
	install: true,
)

# D-Bus starts augurd when a program first asks for it: through systemd,
# as a user service, where there is systemd, else itself.
paths = {'libexecdir': get_option('prefix') / get_option('libexecdir')}
configure_file(
	input: 'data/org.gemwm.Augur.service.in',
	output: 'org.gemwm.Augur.service',
	configuration: paths,
	install_dir: get_option('datadir') / 'dbus-1' / 'services',
)
systemd_user_unit_dir = get_option('systemduserunitdir')
if systemd_user_unit_dir == ''
	systemd = dependency('systemd', required: false)
	if systemd.found()
		systemd_user_unit_dir = systemd.get_variable(
			pkgconfig: 'systemd_user_unit_dir',
			pkgconfig_define: ['prefix', get_option('prefix')])
	elif host_machine.system() == 'linux'
		systemd_user_unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
	else
		systemd_user_unit_dir = 'no'   # the BSDs: no systemd to use it
	endif
endif
if systemd_user_unit_dir != 'no'
	configure_file(
		input: 'data/augurd.service.in',
		output: 'augurd.service',
		configuration: paths,
		install_dir: systemd_user_unit_dir,
	)
endif

install_man(configure_file(
	input: 'data/augur.1.in',
	output: 'augur.1',
	configuration: {'version': meson.project_version()},
))

test_schema = executable('test_schema', ['tests/test_schema.c', 'src/schema.c'],
	dependencies: [glib, json, libm], include_directories: 'src')
test('schema', test_schema)

# The whole service, on a bus of its own, against a pretend provider.
python = find_program('python3', required: false)
dbus_run_session = find_program('dbus-run-session', required: false)
if python.found() and dbus_run_session.found()
	test('service', find_program('tests/test_service.sh'),
		args: [augurd.full_path(), augur.full_path(),
			meson.current_source_dir() / 'tests' / 'mock_provider.py',
			python.full_path()],
		depends: [augurd, augur], timeout: 60, is_parallel: false)
else
	warning('the service test needs python3 and dbus-run-session: not run')
endif
