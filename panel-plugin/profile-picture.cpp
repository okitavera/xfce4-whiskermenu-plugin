/*
 * Copyright (C) 2014, 2016, 2017 Graeme Gott <graeme@gottcode.org>
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

#include "profile-picture.h"

#include "command.h"
#include "settings.h"
#include "slot.h"
#include "window.h"

#include <libxfce4panel/libxfce4panel.h>

using namespace WhiskerMenu;

//-----------------------------------------------------------------------------

ProfilePicture::ProfilePicture(Window* window) :
	m_window(window)
{
	m_image = gtk_image_new();

	gtk_widget_set_halign(m_image, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(m_image, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_start(m_image, 10);
	gtk_widget_set_margin_end(m_image, 10);

	m_container = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(m_container), false);
	gtk_widget_add_events(m_container, GDK_BUTTON_PRESS_MASK);
	g_signal_connect_slot<GtkWidget*, GdkEvent*>(m_container, "button-press-event", &ProfilePicture::on_button_press_event, this);
	gtk_container_add(GTK_CONTAINER(m_container), m_image);

	gchar* path = g_build_filename(g_get_home_dir(), ".face", NULL);
	GFile* file = g_file_new_for_path(path);
	g_free(path);

	m_file_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
	g_signal_connect_slot(m_file_monitor, "changed", &ProfilePicture::on_file_changed, this);
	on_file_changed(m_file_monitor, file, NULL, G_FILE_MONITOR_EVENT_CHANGED);

	g_object_unref(file);
}

//-----------------------------------------------------------------------------

ProfilePicture::~ProfilePicture()
{
	g_file_monitor_cancel(m_file_monitor);
	g_object_unref(m_file_monitor);
}

//-----------------------------------------------------------------------------

void ProfilePicture::on_file_changed(GFileMonitor*, GFile* file, GFile*, GFileMonitorEvent)
{
	if (g_file_query_exists(file, NULL))
	{
		gchar* icon = g_file_get_path(file);
		int iconsize = wm_settings->profile_photo_size;
		GdkPixbuf* iconbuf = gdk_pixbuf_new_from_file_at_size(icon, iconsize, iconsize, NULL);
		gtk_image_set_from_pixbuf(GTK_IMAGE(m_image), iconbuf);
		g_object_unref(iconbuf);
	}
	else
	{
		gtk_image_set_from_icon_name(GTK_IMAGE(m_image), "avatar-default", GTK_ICON_SIZE_DND);
	}
}

//-----------------------------------------------------------------------------

void ProfilePicture::on_button_press_event()
{
	Command* command = wm_settings->command[Settings::CommandProfile];
	if (!command->get_shown())
	{
		return;
	}

	m_window->hide();
	command->activate();
}

//-----------------------------------------------------------------------------
