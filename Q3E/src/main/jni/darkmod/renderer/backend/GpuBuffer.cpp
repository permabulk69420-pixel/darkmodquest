/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/
#include "precompiled.h"
#include "renderer/backend/GpuBuffer.h"
#include "renderer/QuestFrameTiming.h"

extern idCVarBool r_usePersistentMapping;
idCVar r_gpuBufferNonpersistentUpdateMode(
	"r_gpuBufferNonpersistentUpdateMode", "0", CVAR_ARCHIVE | CVAR_RENDERER,
	"When r_usePersistentMapping is off or unsupported, this cvar defines how to update GpuBuffer:\n"
	"   0 --- glBufferSubData\n"
	"   1 --- glMapBufferRange + memcpy + glUnmapBuffer"
);

const int GpuBuffer::NUM_FRAMES;

void GpuBuffer::Init( GLenum type, GLuint size, GLuint alignment, bool retainPreviousFrame, bool isolateFrames ) {
	if( bufferObject ) {
		Destroy();
	}

	frameSize = ALIGN( size, alignment );
	this->alignment = alignment;
	this->type = type;
	this->retainPreviousFrame = retainPreviousFrame;
	numFrames = NUM_FRAMES + (retainPreviousFrame ? 1 : 0);

	usesPersistentMapping = r_usePersistentMapping && GLAD_GL_ARB_buffer_storage;
	isolatedFrames = isolateFrames && !usesPersistentMapping;

	totalSize = numFrames * frameSize;
	if (isolatedFrames) {
		// Same total capacity as the ring, but distinct GL object dependencies.
		// A driver need not synchronize an upload against reads in OTHER frames.
		for (int frame = 0; frame < numFrames; ++frame) {
			qglGenBuffers(1, &frameBuffers[frame]);
			qglBindBuffer(type, frameBuffers[frame]);
			qglBufferData(type, frameSize, nullptr, GL_DYNAMIC_DRAW);
		}
		bufferObject = frameBuffers[0];
		bufferContents = (byte *)Mem_Alloc16(totalSize);
	} else {
		qglGenBuffers(1, &bufferObject);
		qglBindBuffer(type, bufferObject);
		if ( usesPersistentMapping ) {
			qglBufferStorage( type, totalSize, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT );
			bufferContents = ( byte* )qglMapBufferRange( type, 0, totalSize, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT );
		} else {
			qglBufferData( type, totalSize, nullptr, GL_DYNAMIC_DRAW );
			bufferContents = ( byte* )Mem_Alloc16( totalSize );
		}
	}

	currentWritingFrame = 0;
	currentDrawingFrame = 0;
	bytesCommittedInCurrentFrame = 0;
}

void GpuBuffer::InitWriteFrameAhead( GLenum type, GLuint size, GLuint alignment, bool retainPreviousFrame, bool isolateFrames ) {
	Init( type, size, alignment, retainPreviousFrame, isolateFrames );
	currentWritingFrame = 1;
}


void GpuBuffer::Destroy() {
	if ( bufferObject == 0 ) {
		return;
	}

	for (int i = 0; i < NUM_FRAMES + 1; ++i) {
		if ( frameFences[i] != nullptr ) {
			qglDeleteSync( frameFences[i] );
			frameFences[i] = nullptr;
		}
	}

	if ( usesPersistentMapping ) {
		qglBindBuffer( type, bufferObject );
		qglUnmapBuffer( type );
	} else {
		Mem_Free16( bufferContents );
	}

	if (isolatedFrames) {
		for (int frame = 0; frame < numFrames; ++frame) {
			qglDeleteBuffers(1, &frameBuffers[frame]); frameBuffers[frame] = 0;
		}
	} else qglDeleteBuffers( 1, &bufferObject );
	bufferObject = 0;
	bufferContents = nullptr;
	isolatedFrames = false;
}

byte * GpuBuffer::CurrentWriteLocation() const {
	return bufferContents + CurrentOffset();
}

GLuint GpuBuffer::BytesRemaining() const {
	return frameSize - bytesCommittedInCurrentFrame;
}

void GpuBuffer::Commit( GLuint numBytes ) {
	GLuint alignedSize = ALIGN( numBytes, alignment );
	assert( alignedSize + bytesCommittedInCurrentFrame <= frameSize );
	if (!alignedSize) return;

	// for persistent mapping, nothing to do. Otherwise, we need to upload the committed data
	if( !usesPersistentMapping ) {
		GLuint currentOffset = CurrentOffset();
		const GLuint physicalOffset = PhysicalOffset(currentOffset);
		qglBindBuffer( type, GetAPIObjectForOffset(currentOffset) );
		if (r_gpuBufferNonpersistentUpdateMode.GetInteger() == 0)
			qglBufferSubData( type, physicalOffset, alignedSize, bufferContents + currentOffset );
		else {
			void *dst = qglMapBufferRange( type, physicalOffset, alignedSize, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT );
			memcpy(dst, bufferContents + currentOffset, alignedSize);
			qglUnmapBuffer( type );
		}
	}

	bytesCommittedInCurrentFrame += alignedSize;
}

void GpuBuffer::BindRangeToIndexTarget( GLuint index, byte *offset, GLuint size ) {
	GLuint alignedSize = ALIGN( size, alignment );
	GLintptr mapOffset = offset - bufferContents;
	assert(mapOffset >= 0 && mapOffset + alignedSize <= totalSize);
	assert(!isolatedFrames || PhysicalOffset(GLuint(mapOffset)) + alignedSize <= frameSize);
	qglBindBufferRange( type, index, GetAPIObjectForOffset(GLuint(mapOffset)), PhysicalOffset(GLuint(mapOffset)), alignedSize );
}

void GpuBuffer::Bind() {
	qglBindBuffer(type, isolatedFrames ? frameBuffers[currentWritingFrame] : bufferObject);
}

const void * GpuBuffer::BufferOffset( const void *pointer ) {
	ptrdiff_t mapOffset = static_cast< const byte* >( pointer ) - bufferContents;
	assert( (size_t)mapOffset < (size_t)totalSize );
	return reinterpret_cast< const void* >( size_t(PhysicalOffset(GLuint(mapOffset))) );
}

void GpuBuffer::SwitchFrame() {
	if (retainPreviousFrame) {
		// SpaceWarp also reads the preceding frame's animated vertices. Its
		// original fence predates those reads, so replace it with one covering
		// this frame's motion pass. A fourth ring region keeps the next frontend
		// write away from both current and previous GPU inputs.
		const int previous = (currentDrawingFrame + numFrames - 1) % numFrames;
		if (frameFences[previous]) qglDeleteSync(frameFences[previous]);
		frameFences[previous] = qglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}
	// lock current frame contents in buffer
	assert( frameFences[currentDrawingFrame] == nullptr );
	frameFences[currentDrawingFrame] = qglFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );

	currentDrawingFrame = ( currentDrawingFrame + 1 ) % numFrames;
	currentWritingFrame = ( currentWritingFrame + 1 ) % numFrames;
	bytesCommittedInCurrentFrame = 0;

	if ( frameFences[currentWritingFrame] != nullptr ) {
		QuestFrameTiming::Scope timing(QuestTiming(), QuestFrameTiming::FenceWait);
		// await lock for next frame region to ensure that data is not used by the GPU anymore
		GLenum result = qglClientWaitSync( frameFences[currentWritingFrame], 0, 0 );
		while( result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED ) {
			result = qglClientWaitSync( frameFences[currentWritingFrame], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000 );
			if( result == GL_WAIT_FAILED ) {
				assert( !"glClientWaitSync failed" );
				break;
			}
		}
		qglDeleteSync( frameFences[currentWritingFrame] );
		frameFences[currentWritingFrame] = nullptr;
	}
}

GLuint GpuBuffer::CurrentOffset() const {
	return currentWritingFrame * frameSize + bytesCommittedInCurrentFrame;
}
