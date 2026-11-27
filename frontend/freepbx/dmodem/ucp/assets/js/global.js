// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 D-Modem Authors
//
// ucp/assets/js/global.js - the Fax widget of UCP in the browser
// (ucp/Dmodem.class.php makes its HTML): the folder tabs, the buttons of
// each fax (view, download, forward, delete), cancel, the send form, and a
// browser notification when a new fax comes (the poll).

var DmodemC = UCPMC.extend({
	/* the module: no fax seen yet */
	init: function() {
		this.latest = null;
	},

	/* the poll asks for data only while a Fax widget is open */
	prepoll: function() {
		return $(".grid-stack-item[data-rawname=dmodem]").length > 0;
	},

	/* the poll's answer: a notification for a received fax that is
	   newer than the last one seen, then the widgets drawn again */
	poll: function(data) {
		if (!data || !data.latest) {
			return;
		}
		var l = data.latest;
		if (this.latest !== null && l.uniqueid !== "" && l.uniqueid !== this.latest) {
			if (UCP.notify && typeof Notify !== "undefined") {
				new Notify(_("New fax"), {
					body: _("A fax came from") + " " + l.from,
					timeout: 10
				}).show();
			}
			this.refresh();
		}
		this.latest = l.uniqueid;
	},

	/* the Ajax URL of command for fax uniqueid u */
	url: function(command, u) {
		return UCP.ajaxUrl + "?module=dmodem&command=" + command +
			"&uniqueid=" + encodeURIComponent(u);
	},

	/* every Fax widget drawn again from the server */
	refresh: function() {
		var self = this;
		$.post(UCP.ajaxUrl, {module: "dmodem", command: "faxlist"}, function(data) {
			self.draw(data);
		});
	},

	/* an Ajax answer: its message, and the widgets' new HTML */
	draw: function(data) {
		if (data && data.message) {
			UCP.showAlert(data.message, data.status ? "success" : "danger");
		}
		if (data && typeof data.html === "string") {
			$(".grid-stack-item[data-rawname=dmodem] .widget-content").html(data.html);
		}
	},

	/* command with the fields f to the server, then the widgets drawn */
	post: function(command, f) {
		var self = this;
		$.post(UCP.ajaxUrl, $.extend({module: "dmodem", command: command}, f), function(data) {
			self.draw(data);
		});
	},

	/* the widget widget_id shown: its tabs, buttons and form work */
	displayWidget: function(widget_id, dashboard_id) {
		var self = this,
			w = $(".grid-stack-item[data-id='" + widget_id + "'][data-rawname=dmodem]");
		w.off(".dmodem");
		w.on("click.dmodem", "[data-dmodem-tab]", function(e) {
			e.preventDefault();
			var tab = $(this).data("dmodem-tab");
			w.find("[data-dmodem-tab]").removeClass("active");
			$(this).addClass("active");
			w.find("[data-dmodem-pane]").hide();
			w.find("[data-dmodem-pane='" + tab + "']").show();
		});
		w.on("click.dmodem", "[data-dmodem-action]", function() {
			var action = $(this).data("dmodem-action"),
				u = $(this).closest("tr").data("uniqueid"),
				job = $(this).closest("tr").data("job");
			if (action === "faxpdf") {
				window.open(self.url("faxpdf", u), "_blank");
			} else if (action === "faxtiff") {
				window.location.href = self.url("faxtiff", u);
			} else if (action === "faxdelete") {
				if (confirm(_("Delete this fax?"))) {
					self.post("faxdelete", {uniqueid: u});
				}
			} else if (action === "faxforward") {
				var number = prompt(_("Forward to the fax number:"));
				if (number) {
					self.post("faxforward", {uniqueid: u, number: number});
				}
			} else if (action === "faxcancel") {
				self.post("faxcancel", {job: job});
			}
		});
		w.on("submit.dmodem", "form.dmodem-fax-send", function(e) {
			e.preventDefault();
			var fd = new FormData(this);
			fd.append("module", "dmodem");
			fd.append("command", "faxsend");
			$(this).find("button[type=submit]").prop("disabled", true);
			$.ajax({url: UCP.ajaxUrl, type: "POST", data: fd, processData: false,
				contentType: false}).done(function(data) {
				self.draw(data);
			}).fail(function() {
				UCP.showAlert(_("The fax was not sent to the server"), "danger");
			});
		});
	}
});
