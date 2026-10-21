/*
	PSSwanStation - cloud saves: Google Drive or Dropbox as a place to keep the
	memory cards and save states (cloud.cpp; netfiles.cpp keeps them the same).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "syncstore.h"

#include <memory>
#include <string>

namespace fe::cloud
{
enum Service
{
	None = 0,
	GoogleDrive = 1,
	Dropbox = 2,
};
std::string serviceName(Service service);
// Whether this build (or <root>cloud-keys.cfg) has the app's registration with it.
bool available(Service service);
// The service signed in to (None when none), and the account's address.
Service service();
std::string account();

// Signing in: started here, followed by signIn() every frame.
struct SignIn
{
	enum Stage
	{
		Idle,
		Asking,		// a request is out
		Waiting,	// for the user, on the phone: `url` (and, for Google, `code` to type there)
		Done,
		Failed,		// `why`
	};
	Service service = None;
	Stage stage = Idle;
	std::string url, code, why;
};
void startSignIn(Service service);
// Dropbox: the code its page showed once it was allowed.
void dropboxCode(const std::string& code);
SignIn signIn();
void cancelSignIn();
void signOut();

// The storage of the account signed in to, for one run of netfiles.cpp; null
// when signed in to none.
std::unique_ptr<Store> openStore();
// Waits for a sign-in in progress to stop (the title closes).
void finish();
}
