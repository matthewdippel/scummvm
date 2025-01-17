/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * This file is based on WME Lite.
 * http://dead-code.org/redir.php?target=wmelite
 * Copyright (c) 2011 Jan Nedoma
 */

#include "engines/wintermute/base/base_game.h"
#include "engines/wintermute/ad/ad_node_state.h"
#include "engines/wintermute/ad/ad_entity.h"
#include "engines/wintermute/base/base_sprite.h"
#include "engines/wintermute/utils/utils.h"
#include "engines/wintermute/platform_osystem.h"

#include "common/str.h"

namespace Wintermute {

IMPLEMENT_PERSISTENT(AdNodeState, false)


//////////////////////////////////////////////////////////////////////////
AdNodeState::AdNodeState(BaseGame *inGame) : BaseClass(inGame) {
	_name = nullptr;
	_active = false;
	for (int i = 0; i < 7; i++) {
		_caption[i] = nullptr;
	}
	_alphaColor = 0;
	_filename = nullptr;
	_cursor = nullptr;
}


//////////////////////////////////////////////////////////////////////////
AdNodeState::~AdNodeState() {
	delete[] _name;
	delete[] _filename;
	delete[] _cursor;
	_name = nullptr;
	_filename = nullptr;
	_cursor = nullptr;
	for (int i = 0; i < 7; i++) {
		delete[] _caption[i];
		_caption[i] = nullptr;
	}
}


//////////////////////////////////////////////////////////////////////////
void AdNodeState::setName(const char *name) {
	delete[] _name;
	_name = nullptr;
	BaseUtils::setString(&_name, name);
}


//////////////////////////////////////////////////////////////////////////
void AdNodeState::setFilename(const char *filename) {
	delete[] _filename;
	_filename = nullptr;
	BaseUtils::setString(&_filename, filename);
}


//////////////////////////////////////////////////////////////////////////
void AdNodeState::setCursor(const char *filename) {
	delete[] _cursor;
	_cursor = nullptr;
	BaseUtils::setString(&_cursor, filename);
}


//////////////////////////////////////////////////////////////////////////
bool AdNodeState::persist(BasePersistenceManager *persistMgr) {
	persistMgr->transferPtr(TMEMBER_PTR(_gameRef));

	persistMgr->transferBool(TMEMBER(_active));
	persistMgr->transferCharPtr(TMEMBER(_name));
	persistMgr->transferCharPtr(TMEMBER(_filename));
	persistMgr->transferCharPtr(TMEMBER(_cursor));
	persistMgr->transferUint32(TMEMBER(_alphaColor));
	for (int i = 0; i < 7; i++) {
		persistMgr->transferCharPtr(TMEMBER(_caption[i]));
	}

	return STATUS_OK;
}


//////////////////////////////////////////////////////////////////////////
void AdNodeState::setCaption(const char *caption, int caseVal) {
	if (caseVal == 0) {
		caseVal = 1;
	}
	if (caseVal < 1 || caseVal > 7) {
		return;
	}

	delete[] _caption[caseVal - 1];
	_caption[caseVal - 1] = new char[strlen(caption) + 1];
	if (_caption[caseVal - 1]) {
		strcpy(_caption[caseVal - 1], caption);
		_gameRef->expandStringByStringTable(&_caption[caseVal - 1]);
	}
}


//////////////////////////////////////////////////////////////////////////
const char *AdNodeState::getCaption(int caseVal) {
	if (caseVal == 0) {
		caseVal = 1;
	}
	if (caseVal < 1 || caseVal > 7 || _caption[caseVal - 1] == nullptr) {
		return "";
	} else {
		return _caption[caseVal - 1];
	}
}


//////////////////////////////////////////////////////////////////////////
bool AdNodeState::transferEntity(AdEntity *entity, bool includingSprites, bool saving) {
	if (!entity) {
		return STATUS_FAILED;
	}

	// HACK!
	if (this->_gameRef != entity->_gameRef) {
		this->_gameRef = entity->_gameRef;
	}

	if (saving) {
		for (int i = 0; i < 7; i++) {
			if (entity->_caption[i]) {
				setCaption(entity->_caption[i], i);
			}
		}
		if (!entity->_region && entity->_sprite && entity->_sprite->getFilename()) {
			if (includingSprites) {
				setFilename(entity->_sprite->getFilename());
			} else {
				setFilename("");
			}
		}
		if (entity->_cursor && entity->_cursor->getFilename()) {
			setCursor(entity->_cursor->getFilename());
		}
		_alphaColor = entity->_alphaColor;
		_active = entity->_active;
	} else {
		for (int i = 0; i < 7; i++) {
			if (_caption[i]) {
				entity->setCaption(_caption[i], i);
			}
		}
		if (_filename && !entity->_region && includingSprites && strcmp(_filename, "") != 0) {
			if (!entity->_sprite || !entity->_sprite->getFilename() || scumm_stricmp(entity->_sprite->getFilename(), _filename) != 0) {
				entity->setSprite(_filename);
			}
		}
		if (_cursor) {
			if (!entity->_cursor || !entity->_cursor->getFilename() || scumm_stricmp(entity->_cursor->getFilename(), _cursor) != 0) {
				entity->setCursor(_cursor);
			}
		}

		entity->_active = _active;
		entity->_alphaColor = _alphaColor;
	}

	return STATUS_OK;
}

} // End of namespace Wintermute
