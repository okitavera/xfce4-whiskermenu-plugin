/*
 * Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018 Graeme Gott <graeme@gottcode.org>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "plugin.h"

#include "applications-page.h"
#include "command.h"
#include "configuration-dialog.h"
#include "settings.h"
#include "slot.h"
#include "window.h"

extern "C"
{
#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>
}

using namespace WhiskerMenu;

//-----------------------------------------------------------------------------

extern "C" void whiskermenu_construct(XfcePanelPlugin* plugin)
{
	xfce_textdomain(GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");
	new Plugin(plugin);
}

static void whiskermenu_free(XfcePanelPlugin*, Plugin* whiskermenu)
{
	delete whiskermenu;
}

// Wait for grab; allows modifier as shortcut
// Adapted from http://git.xfce.org/xfce/xfce4-panel/tree/common/panel-utils.c#n122
static bool panel_utils_grab_available()
{
	bool grab_succeed = false;

	GdkWindow* root = gdk_screen_get_root_window(xfce_gdk_screen_get_active(NULL));
	GdkDisplay* display = gdk_display_get_default();
	GdkSeat* seat = gdk_display_get_default_seat(display);

	// Don't try to get the grab for longer then 1/4 second
	for (guint i = 0; i < (G_USEC_PER_SEC / 400); ++i)
	{
		if (gdk_seat_grab(seat, root, GDK_SEAT_CAPABILITY_ALL, true, NULL, NULL, NULL, NULL))
		{
			gdk_seat_ungrab(seat);
			grab_succeed = true;
			break;
		}
		g_usleep(100);
	}

	if (!grab_succeed)
	{
		g_printerr("xfce4-whiskermenu-plugin: Unable to get keyboard and mouse grab. Menu popup failed.\n");
	}

	return grab_succeed;
}

static void widget_add_css(GtkWidget* widget, const gchar* css)
{
	GtkCssProvider* provider = gtk_css_provider_new();
	gtk_css_provider_load_from_data(provider, css, -1, NULL);
	gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
			GTK_STYLE_PROVIDER(provider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

//-----------------------------------------------------------------------------

Plugin::Plugin(XfcePanelPlugin* plugin) :
	m_plugin(plugin),
	m_window(NULL),
	m_opacity(100),
	m_file_icon(false),
	m_profile_photo_size(72)
{
	// Load settings
	wm_settings = new Settings;
	wm_settings->button_title = get_button_title_default();
	for (int i = Settings::CommandSwitchUser; i < Settings::CommandLogOut; ++i)
	{
		wm_settings->command[i]->set_shown(false);
	}
	wm_settings->load(xfce_resource_lookup(XFCE_RESOURCE_CONFIG, "xfce4/whiskermenu/defaults.rc"));
	wm_settings->load(xfce_panel_plugin_lookup_rc_file(m_plugin));
	m_opacity = wm_settings->menu_opacity;
	m_profile_photo_size = wm_settings->profile_photo_size;

	// Prevent empty panel button
	if (!wm_settings->button_icon_visible)
	{
		if (!wm_settings->button_title_visible)
		{
			wm_settings->button_icon_visible = true;
		}
		else if (wm_settings->button_title.empty())
		{
			wm_settings->button_title = get_button_title_default();
		}
	}

	// Create toggle button
	m_button = xfce_panel_create_toggle_button();
	gtk_widget_set_name(m_button, "whiskermenu-button");
#if !LIBXFCE4PANEL_CHECK_VERSION(4,13,0)
	widget_add_css(m_button, ".xfce4-panel button { padding: 1px; }");
#endif
	g_signal_connect_slot(m_button, "toggled", &Plugin::button_toggled, this);
	gtk_widget_show(m_button);

	m_button_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2));
	gtk_container_add(GTK_CONTAINER(m_button), GTK_WIDGET(m_button_box));
	gtk_container_set_border_width(GTK_CONTAINER(m_button_box), 0);
	gtk_widget_show(GTK_WIDGET(m_button_box));

	m_button_icon = GTK_IMAGE(gtk_image_new());
	icon_changed(wm_settings->button_icon_name.c_str());
	gtk_box_pack_start(m_button_box, GTK_WIDGET(m_button_icon), true, false, 0);
	if (wm_settings->button_icon_visible)
	{
		gtk_widget_show(GTK_WIDGET(m_button_icon));
	}
	gtk_widget_set_sensitive(GTK_WIDGET(m_button_icon), false);

	m_button_label = GTK_LABEL(gtk_label_new(NULL));
	gtk_label_set_markup(m_button_label, wm_settings->button_title.c_str());
	gtk_box_pack_start(m_button_box, GTK_WIDGET(m_button_label), true, true, 0);
	if (wm_settings->button_title_visible)
	{
		gtk_widget_show(GTK_WIDGET(m_button_label));
	}
	gtk_widget_set_sensitive(GTK_WIDGET(m_button_label), false);

	// Add plugin to panel
	gtk_container_add(GTK_CONTAINER(plugin), m_button);
	xfce_panel_plugin_add_action_widget(plugin, m_button);

	// Connect plugin signals to functions
	g_signal_connect(plugin, "free-data", G_CALLBACK(whiskermenu_free), this);
	g_signal_connect_slot<XfcePanelPlugin*>(plugin, "configure-plugin", &Plugin::configure, this);
	g_signal_connect_slot(plugin, "mode-changed", &Plugin::mode_changed, this);
	g_signal_connect_slot(plugin, "remote-event", &Plugin::remote_event, this);
	g_signal_connect_slot<XfcePanelPlugin*>(plugin, "save", &Plugin::save, this);
	g_signal_connect_slot<XfcePanelPlugin*>(plugin, "about", &Plugin::show_about, this);
	g_signal_connect_slot(plugin, "size-changed", &Plugin::size_changed, this);

	xfce_panel_plugin_menu_show_about(plugin);
	xfce_panel_plugin_menu_show_configure(plugin);
	xfce_panel_plugin_menu_insert_item(plugin, GTK_MENU_ITEM(wm_settings->command[Settings::CommandMenuEditor]->get_menuitem()));

	mode_changed(m_plugin, xfce_panel_plugin_get_mode(m_plugin));

	g_signal_connect_slot<GtkWidget*,GtkStyle*>(m_button, "style-set", &Plugin::update_size, this);
	g_signal_connect_slot<GtkWidget*,GdkScreen*>(m_button, "screen-changed", &Plugin::update_size, this);

	// Create menu window
	m_window = new Window(this);
	g_signal_connect_slot<GtkWidget*>(m_window->get_widget(), "unmap", &Plugin::menu_hidden, this);
}

//-----------------------------------------------------------------------------

Plugin::~Plugin()
{
	save();

	delete m_window;
	m_window = NULL;

	gtk_widget_destroy(m_button);

	delete wm_settings;
	wm_settings = NULL;
}

//-----------------------------------------------------------------------------

Plugin::ButtonStyle Plugin::get_button_style() const
{
	return ButtonStyle(wm_settings->button_icon_visible | (wm_settings->button_title_visible << 1));
}

//-----------------------------------------------------------------------------

std::string Plugin::get_button_title() const
{
	return wm_settings->button_title;
}

//-----------------------------------------------------------------------------

std::string Plugin::get_button_title_default()
{
	return _("Applications");
}

//-----------------------------------------------------------------------------

std::string Plugin::get_button_icon_name() const
{
	return wm_settings->button_icon_name;
}

//-----------------------------------------------------------------------------

void Plugin::reload()
{
	m_window->hide();
	m_window->get_applications()->invalidate_applications();
}

//-----------------------------------------------------------------------------

void Plugin::set_button_style(ButtonStyle style)
{
	wm_settings->button_icon_visible = style & ShowIcon;
	if (wm_settings->button_icon_visible)
	{
		gtk_widget_show(GTK_WIDGET(m_button_icon));
	}
	else
	{
		gtk_widget_hide(GTK_WIDGET(m_button_icon));
	}

	wm_settings->button_title_visible = style & ShowText;
	if (wm_settings->button_title_visible)
	{
		gtk_widget_show(GTK_WIDGET(m_button_label));
	}
	else
	{
		gtk_widget_hide(GTK_WIDGET(m_button_label));
	}

	wm_settings->set_modified();

	size_changed(m_plugin, xfce_panel_plugin_get_size(m_plugin));
}

//-----------------------------------------------------------------------------

void Plugin::set_button_title(const std::string& title)
{
	wm_settings->button_title = title;
	wm_settings->set_modified();
	gtk_label_set_markup(m_button_label, wm_settings->button_title.c_str());
	size_changed(m_plugin, xfce_panel_plugin_get_size(m_plugin));
}

//-----------------------------------------------------------------------------

void Plugin::set_button_icon_name(const std::string& icon)
{
	wm_settings->button_icon_name = icon;
	wm_settings->set_modified();
	icon_changed(icon.c_str());
	size_changed(m_plugin, xfce_panel_plugin_get_size(m_plugin));
}

//-----------------------------------------------------------------------------

void Plugin::set_configure_enabled(bool enabled)
{
	if (enabled)
	{
		xfce_panel_plugin_unblock_menu(m_plugin);
	}
	else
	{
		xfce_panel_plugin_block_menu(m_plugin);
	}
}

//-----------------------------------------------------------------------------

void Plugin::set_loaded(bool loaded)
{
	gtk_widget_set_sensitive(GTK_WIDGET(m_button_icon), loaded);
	gtk_widget_set_sensitive(GTK_WIDGET(m_button_label), loaded);
}

//-----------------------------------------------------------------------------

void Plugin::button_toggled(GtkToggleButton* button)
{
	if (gtk_toggle_button_get_active(button) == false)
	{
		if (gtk_widget_get_visible(m_window->get_widget()))
		{
			m_window->hide();
		}
		xfce_panel_plugin_block_autohide(m_plugin, false);
	}
	else
	{
		xfce_panel_plugin_block_autohide(m_plugin, true);
		show_menu(false);
	}
}

//-----------------------------------------------------------------------------

void Plugin::menu_hidden()
{
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_button), false);
	save();
}

//-----------------------------------------------------------------------------

void Plugin::configure()
{
	ConfigurationDialog* dialog = new ConfigurationDialog(this);
	g_signal_connect_slot<GtkWidget*>(dialog->get_widget(), "destroy", &Plugin::save, this);
}

//-----------------------------------------------------------------------------

void Plugin::icon_changed(const gchar* icon)
{
	if (!g_path_is_absolute(icon))
	{
		gtk_image_set_from_icon_name(m_button_icon, icon, GTK_ICON_SIZE_BUTTON);
		m_file_icon = false;
	}
	else
	{
		gtk_image_clear(m_button_icon);
		m_file_icon = true;
	}
}

//-----------------------------------------------------------------------------

void Plugin::mode_changed(XfcePanelPlugin*, XfcePanelPluginMode mode)
{
	gtk_label_set_angle(m_button_label, (mode == XFCE_PANEL_PLUGIN_MODE_VERTICAL) ? 270: 0);
	update_size();
}

//-----------------------------------------------------------------------------

gboolean Plugin::remote_event(XfcePanelPlugin*, gchar* name, GValue* value)
{
	if (strcmp(name, "popup") || !panel_utils_grab_available())
	{
		return false;
	}

	if (gtk_widget_get_visible(m_window->get_widget()))
	{
		m_window->hide();
	}
	else if (value && G_VALUE_HOLDS_BOOLEAN(value) && g_value_get_boolean(value))
	{
		show_menu(true);
	}
	else
	{
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_button), true);
	}

	return true;
}

//-----------------------------------------------------------------------------

void Plugin::save()
{
	m_window->save();

	if (wm_settings->get_modified())
	{
		wm_settings->save(xfce_panel_plugin_save_location(m_plugin, true));
	}
}

//-----------------------------------------------------------------------------

void Plugin::show_about()
{
	const gchar* authors[] = {
		"Graeme Gott <graeme@gottcode.org>",
		NULL };

	gtk_show_about_dialog
		(NULL,
		"authors", authors,
		"comments", _("Alternate application launcher for Xfce"),
		"copyright", _("Copyright \302\251 2013-2018 Graeme Gott"),
		"license", XFCE_LICENSE_GPL,
		"logo-icon-name", "xfce4-whiskermenu",
		"program-name", PACKAGE_NAME,
		"translator-credits", _("translator-credits"),
		"version", PACKAGE_VERSION,
		"website", PLUGIN_WEBSITE,
		NULL);
}

//-----------------------------------------------------------------------------

gboolean Plugin::size_changed(XfcePanelPlugin*, gint size)
{
	GtkOrientation panel_orientation = xfce_panel_plugin_get_orientation(m_plugin);
	GtkOrientation orientation = panel_orientation;
	XfcePanelPluginMode mode = xfce_panel_plugin_get_mode(m_plugin);

	// Make icon expand to fill button if title is not visible
	gtk_box_set_child_packing(GTK_BOX(m_button_box), GTK_WIDGET(m_button_icon),
			!wm_settings->button_title_visible,
			!wm_settings->button_title_visible,
			0, GTK_PACK_START);

	// Resize icon
	if (wm_settings->button_single_row)
	{
		size /= xfce_panel_plugin_get_nrows(m_plugin);
	}
#if LIBXFCE4PANEL_CHECK_VERSION(4,13,0)
	gint icon_size = xfce_panel_plugin_get_icon_size(m_plugin);
#else
	GtkBorder padding, border;
	GtkStyleContext* context = gtk_widget_get_style_context(m_button);
	GtkStateFlags flags = gtk_widget_get_state_flags(m_button);
	gtk_style_context_get_padding(context, flags, &padding);
	gtk_style_context_get_border(context, flags, &border);
	gint xthickness = padding.left + padding.right + border.left + border.right;
	gint ythickness = padding.top + padding.bottom + border.top + border.bottom;
	gint icon_size = size - 2 * std::max(xthickness, ythickness);
#endif
	gtk_image_set_pixel_size(m_button_icon, icon_size);

	// Load icon from absolute path
	if (m_file_icon)
	{
		const gint scale = gtk_widget_get_scale_factor(m_button);
		gint max_width = icon_size * scale;
		gint max_height = icon_size * scale;
		if (mode == XFCE_PANEL_PLUGIN_MODE_HORIZONTAL)
		{
			max_width *= 6;
		}
		else
		{
			max_height *= 6;
		}

		GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(wm_settings->button_icon_name.c_str(), max_width, max_height, NULL);
		if (pixbuf)
		{
			// Handle high dpi
			cairo_surface_t* surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
			gtk_image_set_from_surface(m_button_icon, surface);
			cairo_surface_destroy(surface);
			g_object_unref(pixbuf);
		}
	}

	if (wm_settings->button_title_visible)
	{
		gtk_widget_set_size_request(m_button, -1, -1);
	}
	else
	{
		gtk_widget_set_size_request(m_button, size, size);
	}

	if (wm_settings->button_title_visible || !wm_settings->button_single_row)
	{
		xfce_panel_plugin_set_small(m_plugin, false);

		// Put title next to icon if panel is wide enough
		GtkRequisition label_size;
		gtk_widget_get_preferred_size(GTK_WIDGET(m_button_label), NULL, &label_size);
		if (mode == XFCE_PANEL_PLUGIN_MODE_DESKBAR &&
				wm_settings->button_title_visible &&
				wm_settings->button_icon_visible &&
				label_size.width <= (size - icon_size - 4))
		{
			orientation = GTK_ORIENTATION_HORIZONTAL;
		}
	}
	else
	{
		xfce_panel_plugin_set_small(m_plugin, true);
	}

	// Fix alignment in deskbar mode
	if ((panel_orientation == GTK_ORIENTATION_VERTICAL) && (orientation == GTK_ORIENTATION_HORIZONTAL))
	{
		gtk_box_set_child_packing(m_button_box, GTK_WIDGET(m_button_label), false, false, 0, GTK_PACK_START);
	}
	else
	{
		gtk_box_set_child_packing(m_button_box, GTK_WIDGET(m_button_label), true, true, 0, GTK_PACK_START);
	}

	gtk_orientable_set_orientation(GTK_ORIENTABLE(m_button_box), orientation);

	return true;
}

//-----------------------------------------------------------------------------

void Plugin::update_size()
{
	size_changed(m_plugin, xfce_panel_plugin_get_size(m_plugin));
}

//-----------------------------------------------------------------------------

void Plugin::show_menu(bool at_cursor)
{
	if (wm_settings->menu_opacity != m_opacity)
	{
		if ((m_opacity == 100) || (wm_settings->menu_opacity == 100))
		{
			delete m_window;
			m_window = new Window(this);
			g_signal_connect_slot<GtkWidget*>(m_window->get_widget(), "unmap", &Plugin::menu_hidden, this);
		}
		m_opacity = wm_settings->menu_opacity;
	}

	if (wm_settings->profile_photo_size != m_profile_photo_size)
	{
		delete m_window;
		m_window = new Window(this);
		g_signal_connect_slot<GtkWidget*>(m_window->get_widget(), "unmap", &Plugin::menu_hidden, this);
		m_opacity = wm_settings->menu_opacity;
	}
	
	m_window->show(at_cursor ? Window::PositionAtCursor : Window::Position(xfce_panel_plugin_get_orientation(m_plugin)));
}

//-----------------------------------------------------------------------------
