/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once

// Evaluated at declaration load. Fog particles may share a generic smoke
// material with non-fog effects, so keep their identity on the particle stage.
namespace QuestAtmosphere {
inline char Lower( char c ) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

inline bool StartsWith( const char *name, const char *prefix ) {
	if ( !name ) return false;
	while ( *prefix ) {
		if ( Lower(*name++) != Lower(*prefix++) ) return false;
	}
	return true;
}

inline bool Contains( const char *name, const char *word ) {
	if ( !name ) return false;
	for ( ; *name; ++name ) if ( StartsWith(name, word) ) return true;
	return false;
}

inline bool IsFogMaterial( const char *name ) {
	return StartsWith(name, "fogs/") || StartsWith(name, "tdm_fogs/") ||
		StartsWith(name, "textures/darkmod/fogs/") ||
		StartsWith(name, "textures/darkmod/sfx/overcast");
}

inline bool IsFogParticle( const char *name ) {
	if ( Contains(name, "fog") || Contains(name, "overcast") ) return true;
	// Match mist at a word boundary, not unrelated names such as "alchemist".
	if ( name ) for ( const char *p = name; *p; ++p ) {
		char previous = p == name ? 0 : Lower(p[-1]);
		if ( !(previous >= 'a' && previous <= 'z') && StartsWith(p, "mist") ) return true;
	}
	return false;
}
}
