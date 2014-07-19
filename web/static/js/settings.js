
function SettingsStatus(elem) {
	var self = this;
	self.has_changed = ko.observable('')
	self.context = ko.observable('')
	self.type = ko.observable('')
	self.showMore = function() {
		self.showDetails(!self.showDetails());
	}
	self.update = function(elem) {
		self.context(elem['context'])
		self.type(elem['type'])
		self.has_changed(elem['has_changed'])
	}
	
}


function LocalKeyEntry(path) {
	var self = this;
	self.value = ko.observable('')
	self.path = path;
	self.key = ''
	self.type = 'string'
}

var refresh_count = 0;
function done_refresh(self, count) {
	if (!count)
		count = 1
	refresh_count-=count
	if (refresh_count < 0)
		refresh_count = 0
	if (refresh_count==0)
		self.nscp_status().not_busy()
}
function init_refresh(self) {
	self.nscp_status().busy('Refreshing', 'Refreshing data...')
	refresh_count += 2;
}

function CommandViewModel() {
	var self = this;

	self.nscp_status = ko.observable(new NSCPStatus());
	path = getUrlVars()['path']
	if (!path)
		path = '/'
	self.currentPath = ko.observableArray(make_paths_from_string(path))
	self.paths = ko.observableArray([]);
	self.keys = ko.observableArray([]);
	self.akeys = ko.observableArray([]);
	self.current = ko.observable();
	self.status = ko.observable(new SettingsStatus());
	self.addNew = ko.observable(new LocalKeyEntry(path));

	init_refresh(self);
	self.showAddKey = function(command) {
		$("#addKey").modal('show');
	}
	self.toggleAdvanced = function(command) {
		$("#adkeys").modal($('#myModal').hasClass('in')?'hide':'show');
	}
	self.addNewKey = function(command) {
		init_refresh(self);
		root={}
		root['header'] = {};
		root['header']['version'] = 1;
		root['type'] = 'SettingsRequestMessage';
		root['payload'] = [build_settings_payload(self.addNew())];

		$.post("/settings/query.json", JSON.stringify(root), function(data) {
			self.refresh()
		})
	}
	self.save = function(command) {
		init_refresh(self);
		root={}
		root['header'] = {};
		root['header']['version'] = 1;
		root['type'] = 'SettingsRequestMessage';
		root['payload'] = [];
		self.keys().forEach(function(entry) {
			if (entry.old_value != entry.value())
				root['payload'].push(entry.build_payload())
		})
		self.akeys().forEach(function(entry) {
			if (entry.old_value != entry.value())
				root['payload'].push(entry.build_payload())
		})
		if (root['payload'].length > 0) {
			$.post("/settings/query.json", JSON.stringify(root), function(data) {
				self.refresh()
			})
		} else {
			self.nscp_status().message("warn", "Settings not saved", "No changes detected");
			done_refresh(self, 2);
		}
	}
	self.loadStore = function(command) {
		init_refresh(self);
		root={}
		root['header'] = {};
		root['header']['version'] = 1;
		root['type'] = 'SettingsRequestMessage';
		payload = {}
		payload['plugin_id'] = 1234
		payload['control'] = {}
		payload['control']['command'] = 'LOAD'
		root['payload'] = [ payload ];
		
		$.post("/settings/query.json", JSON.stringify(root), function(data) {
			self.refresh()
		})
	}
	self.saveStore = function(command) {
		init_refresh(self);
		root={}
		root['header'] = {};
		root['header']['version'] = 1;
		root['type'] = 'SettingsRequestMessage';
		payload = {}
		payload['plugin_id'] = 1234
		payload['control'] = {}
		payload['control']['command'] = 'SAVE'
		
		root['payload'] = [ payload ];
		$.post("/settings/query.json", JSON.stringify(root), function(data) {
			self.refresh()
		})
	}
	self.refresh = function() {
		$.getJSON("/settings/inventory?path=" + path + "&recursive=true&paths=true", function(data) {
			self.paths.removeAll()
			if (data['payload'][0]['inventory']) {
				data['payload'][0]['inventory'].forEach(function(entry) {
					if (path == entry['node']['path']) {
						self.current(new PathEntry(entry))
					} else {
						self.paths.push(new PathEntry(entry));
					}
				});
			}
			done_refresh(self);
		})
		$.getJSON("/settings/inventory?path=" + path + "&recursive=false&keys=true", function(data) {
			self.keys.removeAll()
			self.akeys.removeAll()
			if (data['payload'][0]['inventory']) {
				data['payload'][0]['inventory'].forEach(function(entry) {
					key = new KeyEntry(entry)
					if (key.advanced)
						self.akeys.push(key);
					else
						self.keys.push(key);
				});
			}
			done_refresh(self);
		})
		self.keys.sort(function(left, right) { return left.name == right.name ? 0 : (left.name < right.name ? -1 : 1) })
		self.akeys.sort(function(left, right) { return left.name == right.name ? 0 : (left.name < right.name ? -1 : 1) })
		$.getJSON("/settings/status", function(data) {
			self.status().update(data['payload'][0]['status'])
		})
	}
	self.set_default_value = function(key) {
		key.value('') // key.default_value
	}
	self.refresh()
}
ko.applyBindings(new CommandViewModel());

