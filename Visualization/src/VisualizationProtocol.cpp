/* -------------------------------------------------------------------------- *
 *                      SimTK Core: SimTK Simbody(tm)                         *
 * -------------------------------------------------------------------------- *
 * This is part of the SimTK Core biosimulation toolkit originating from      *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org.               *
 *                                                                            *
 * Portions copyright (c) 2010 Stanford University and the Authors.           *
 * Authors: Peter Eastman                                                     *
 * Contributors: Michael Sherman                                              *     
 *                                                                            *
 * Permission is hereby granted, free of charge, to any person obtaining a    *
 * copy of this software and associated documentation files (the "Software"), *
 * to deal in the Software without restriction, including without limitation  *
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,   *
 * and/or sell copies of the Software, and to permit persons to whom the      *
 * Software is furnished to do so, subject to the following conditions:       *
 *                                                                            *
 * The above copyright notice and this permission notice shall be included in *
 * all copies or substantial portions of the Software.                        *
 *                                                                            *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    *
 * THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,    *
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR      *
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE  *
 * USE OR OTHER DEALINGS IN THE SOFTWARE.                                     *
 * -------------------------------------------------------------------------- */


#include "simbody/internal/common.h"
#include "simbody/internal/Visualizer.h"
#include "simbody/internal/Visualizer_EventListener.h"
#include "VisualizationProtocol.h"

#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <string>

using namespace SimTK;
using namespace std;

#ifdef _WIN32
    #include <fcntl.h>
    #include <io.h>
    #include <process.h>
    #define READ _read
#else
    #include <unistd.h>
    #define READ read
#endif

// gcc 4.4.3 complains bitterly if you don't check the return
// status from the write() system call. This avoids those 
// warnings and maybe, someday, will catch an error.
#define WRITE(pipeno, buf, len) \
   {int status=write((pipeno), (buf), (len)); \
    SimTK_ERRCHK4_ALWAYS(status!=-1, "VisualizationProtocol",  \
    "An attempt to write() %d bytes to pipe %d failed with errno=%d (%s).", \
    (len),(pipeno),errno,strerror(errno));}

static int inPipe;

// Create a pipe, using the right call for this platform.
static int createPipe(int pipeHandles[2]) {
    const int status =
#ifdef _WIN32
        _pipe(pipeHandles, 16384, _O_BINARY);
#else
        pipe(pipeHandles);
#endif
    return status;
}

// Spawn the visualizer GUI executable, using the right method for
// this platform. We take two executables to try in order,
// and return after the first one succeeds. If neither works, we throw
// an error that is hopefully helful.
static void spawnViz(const char* localPath, const char* installPath,
                    const char* appName, int toSimPipe, int fromSimPipe,
                    const char* title)
{
    int status;
    char vizPipeToSim[32], vizPipeFromSim[32];
    sprintf(vizPipeToSim, "%d", toSimPipe);
    sprintf(vizPipeFromSim, "%d", fromSimPipe);

#ifdef _WIN32
    intptr_t handle;
    handle = _spawnl(P_NOWAIT, localPath, appName, 
                     vizPipeToSim, vizPipeFromSim, title, (const char*)0);
    if (handle == -1)
        handle = _spawnl(P_NOWAIT, installPath, appName, 
                         vizPipeToSim, vizPipeFromSim, title, (const char*)0);
    status = (handle==-1) ? -1 : 0;
#else
    const pid_t pid = fork();
    if (pid == 0) {
        // child process
        status = execl(localPath, appName, vizPipeToSim, vizPipeFromSim, title,
                       (const char*)0); 
        // if we get here the execl() failed
        status = execl(installPath, appName, vizPipeToSim, vizPipeFromSim, title,
                       (const char*)0); 
       // fall through
    } else {
        // parent process
        status = (pid==-1) ? -1 : 0;
    }
#endif

    SimTK_ERRCHK4_ALWAYS(status == 0, "VisualizationProtocol::ctor()",
        "Unable to spawn the Visualization GUI; tried '%s' and '%s'. Got"
        " errno=%d (%s).", 
        localPath, installPath, errno, strerror(errno));
}

static void readData(unsigned char* buffer, int bytes) {
    int totalRead = 0;
    while (totalRead < bytes)
        totalRead += READ(inPipe, buffer+totalRead, bytes-totalRead);
}

static void* listenForVisualizationEvents(void* arg) {
    Visualizer& visualizer = *reinterpret_cast<Visualizer*>(arg);
    unsigned char buffer[256];
    while (true) {
        // Receive an event.

        readData(buffer, 1);
        switch (buffer[0]) {
            case KEY_PRESSED: {
                readData(buffer, 2);
                const Array_<Visualizer::EventListener*>& listeners = visualizer.getEventListeners();
                unsigned keyCode = buffer[0];
                if (buffer[1] & Visualizer::EventListener::IsSpecialKey)
                    keyCode += Visualizer::EventListener::SpecialKeyOffset;
                for (int i = 0; i < (int) listeners.size(); i++)
                    if (listeners[i]->keyPressed(keyCode, (unsigned)(buffer[1])))
                        break; // key press has been handled
                break;
            }
            case MENU_SELECTED: {
                int item;
                readData((unsigned char*) &item, sizeof(int));
                const Array_<Visualizer::EventListener*>& listeners = visualizer.getEventListeners();
                for (int i = 0; i < (int) listeners.size(); i++)
                    if (listeners[i]->menuSelected(item))
                        break; // menu event has been handled
                break;
            }
            default:
                SimTK_ASSERT_ALWAYS(false, "Unexpected data received from visualizer");
        }
    }
    return 0;
}

namespace SimTK {

// Add quotes to string if necessary, so it can be passed safely as a command
// line argument.
static String quoteString(const String& str) {
    String outstr;
    // Escape double quotes, quote whitespace
    bool quoting = false;
    for (int i=0; i < str.size(); ++i) {
        if (std::isspace(str[i])) {
            if (!quoting) {outstr += "\""; quoting=true;}
        } else {
            if (quoting) {outstr += "\""; quoting=false;}
            if (str[i]=='"') outstr += "\\";
        }
        outstr += str[i];
    }
    return outstr;
}

VisualizationProtocol::VisualizationProtocol(Visualizer& visualizer, const String& title) {
    // Launch the GUI application. We'll first look for one in the same directory
    // as the running executable; then if that doesn't work we'll look in the
    // bin subdirectory of the SimTK installation.

    const char* GuiAppName = "VisualizationGUI";
    const String localPath = Pathname::getThisExecutableDirectory() + GuiAppName;
    const String installPath =
        Pathname::addDirectoryOffset(Pathname::getInstallDir("SimTK_INSTALL_DIR", "SimTK"),
                                     "bin") + GuiAppName;

    int sim2vizPipe[2], viz2simPipe[2], status;

    // Create pipe pair for communication from simulator to visualizer.
    status = createPipe(sim2vizPipe);
    SimTK_ASSERT_ALWAYS(status != -1, "VisualizationProtocol: Failed to open pipe");
    outPipe = sim2vizPipe[1];

    // Create pipe pair for communication from visualizer to simulator.
    status = createPipe(viz2simPipe);
    SimTK_ASSERT_ALWAYS(status != -1, "VisualizationProtocol: Failed to open pipe");
    inPipe = viz2simPipe[0];

    // Surround the title argument in quotes so it doesn't look like multiple arguments.
    const String qtitle = quoteString(title);

    // Spawn the visualizer gui, trying local first then installed version.
    spawnViz(localPath.c_str(), installPath.c_str(),
             GuiAppName, sim2vizPipe[0], viz2simPipe[1], qtitle.c_str());

    // Spawn the thread to listen for events.

    pthread_mutex_init(&sceneLock, NULL);
    pthread_t thread;
    pthread_create(&thread, NULL, listenForVisualizationEvents, &visualizer);
}

void VisualizationProtocol::beginScene() {
    pthread_mutex_lock(&sceneLock);
    char command = START_OF_SCENE;
    WRITE(outPipe, &command, 1);
}

void VisualizationProtocol::finishScene() {
    char command = END_OF_SCENE;
    WRITE(outPipe, &command, 1);
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::drawBox(const Transform& transform, const Vec3& scale, const Vec4& color, int representation) {
    drawMesh(transform, scale, color, (short) representation, 0);
}

void VisualizationProtocol::drawEllipsoid(const Transform& transform, const Vec3& scale, const Vec4& color, int representation) {
    drawMesh(transform, scale, color, (short) representation, 1);
}

void VisualizationProtocol::drawCylinder(const Transform& transform, const Vec3& scale, const Vec4& color, int representation) {
    drawMesh(transform, scale, color, (short) representation, 2);
}

void VisualizationProtocol::drawCircle(const Transform& transform, const Vec3& scale, const Vec4& color, int representation) {
    drawMesh(transform, scale, color, (short) representation, 3);
}

void VisualizationProtocol::drawPolygonalMesh(const PolygonalMesh& mesh, const Transform& transform, Real scale, const Vec4& color, int representation) {
    const void* impl = &mesh.getImpl();
    int index;
    map<const void*, int>::const_iterator iter = meshes.find(impl);
    if (iter == meshes.end()) {
        // This is a new mesh, so we need to send it to the visualizer.  Build lists of vertices and faces,
        // triangulating as necessary.

        vector<float> vertices;
        vector<unsigned short> faces;
        for (int i = 0; i < mesh.getNumVertices(); i++) {
            Vec3 pos = mesh.getVertexPosition(i);
            vertices.push_back((float) pos[0]);
            vertices.push_back((float) pos[1]);
            vertices.push_back((float) pos[2]);
        }
        for (int i = 0; i < mesh.getNumFaces(); i++) {
            int numVert = mesh.getNumVerticesForFace(i);
            if (numVert < 3)
                continue; // Ignore it.
            if (numVert == 3) {
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 0));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 1));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 2));
            }
            else if (numVert == 4) {
                // Split it into two triangles.

                faces.push_back((unsigned short) mesh.getFaceVertex(i, 0));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 1));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 2));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 2));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 3));
                faces.push_back((unsigned short) mesh.getFaceVertex(i, 0));
            }
            else {
                // Add a vertex at the center, then split it into triangles.

                Vec3 center(0);
                for (int j = 0; j < numVert; j++)
                    center += vertices[mesh.getFaceVertex(i, j)];
                center /= numVert;
                vertices.push_back((float) center[0]);
                vertices.push_back((float) center[1]);
                vertices.push_back((float) center[2]);
                int newIndex = vertices.size()-1;
                for (int j = 0; j < numVert-1; j++) {
                    faces.push_back((unsigned short) mesh.getFaceVertex(i, j));
                    faces.push_back((unsigned short) mesh.getFaceVertex(i, j+1));
                    faces.push_back((unsigned short) newIndex);
                }
            }
        }
        SimTK_ASSERT_ALWAYS(vertices.size() <= 65536*3, "DecorativeMesh cannot have more than 65536 vertices");
        SimTK_ASSERT_ALWAYS(faces.size() <= 65536*3, "DecorativeMesh cannot have more than 65536 faces");
        index = meshes.size()+4;
        meshes[impl] = index;
        char command = DEFINE_MESH;
        WRITE(outPipe, &command, 1);
        unsigned short numVertices = vertices.size()/3;
        unsigned short numFaces = faces.size()/3;
        WRITE(outPipe, &numVertices, sizeof(short));
        WRITE(outPipe, &numFaces, sizeof(short));
        WRITE(outPipe, &vertices[0], vertices.size()*sizeof(float));
        WRITE(outPipe, &faces[0], faces.size()*sizeof(short));
    }
    else
        index = iter->second;
    drawMesh(transform, Vec3(scale), color, (short) representation, index);
}

void VisualizationProtocol::drawMesh(const Transform& transform, const Vec3& scale, const Vec4& color, short representation, short meshIndex) {
    char command = (representation == DecorativeGeometry::DrawPoints ? ADD_POINT_MESH : (representation == DecorativeGeometry::DrawWireframe ? ADD_WIREFRAME_MESH : ADD_SOLID_MESH));
    WRITE(outPipe, &command, 1);
    float buffer[13];
    Vec3 rot = transform.R().convertRotationToBodyFixedXYZ();
    buffer[0] = (float) rot[0];
    buffer[1] = (float) rot[1];
    buffer[2] = (float) rot[2];
    buffer[3] = (float) transform.T()[0];
    buffer[4] = (float) transform.T()[1];
    buffer[5] = (float) transform.T()[2];
    buffer[6] = (float) scale[0];
    buffer[7] = (float) scale[1];
    buffer[8] = (float) scale[2];
    buffer[9] = (float) color[0];
    buffer[10] = (float) color[1];
    buffer[11] = (float) color[2];
    buffer[12] = (float) color[3];
    WRITE(outPipe, buffer, 13*sizeof(float));
    WRITE(outPipe, &meshIndex, sizeof(short));
}

void VisualizationProtocol::drawLine(const Vec3& end1, const Vec3& end2, const Vec4& color, Real thickness) {
    char command = ADD_LINE;
    WRITE(outPipe, &command, 1);
    float buffer[10];
    buffer[0] = (float) color[0];
    buffer[1] = (float) color[1];
    buffer[2] = (float) color[2];
    buffer[3] = (float) thickness;
    buffer[4] = (float) end1[0];
    buffer[5] = (float) end1[1];
    buffer[6] = (float) end1[2];
    buffer[7] = (float) end2[0];
    buffer[8] = (float) end2[1];
    buffer[9] = (float) end2[2];
    WRITE(outPipe, buffer, 10*sizeof(float));
}

void VisualizationProtocol::drawText(const Vec3& position, Real scale, const Vec4& color, const string& string) {
    SimTK_ASSERT_ALWAYS(string.size() <= 256, "DecorativeText cannot be longer than 256 characters");
    char command = ADD_TEXT;
    WRITE(outPipe, &command, 1);
    float buffer[7];
    buffer[0] = (float) position[0];
    buffer[1] = (float) position[1];
    buffer[2] = (float) position[2];
    buffer[3] = (float) scale;
    buffer[4] = (float) color[0];
    buffer[5] = (float) color[1];
    buffer[6] = (float) color[2];
    WRITE(outPipe, buffer, 7*sizeof(float));
    short length = string.size();
    WRITE(outPipe, &length, sizeof(short));
    WRITE(outPipe, &string[0], length);
}

void VisualizationProtocol::drawFrame(const Transform& transform, Real axisLength, const Vec4& color) {
    char command = ADD_FRAME;
    WRITE(outPipe, &command, 1);
    float buffer[10];
    Vec3 rot = transform.R().convertRotationToBodyFixedXYZ();
    buffer[0] = (float) rot[0];
    buffer[1] = (float) rot[1];
    buffer[2] = (float) rot[2];
    buffer[3] = (float) transform.T()[0];
    buffer[4] = (float) transform.T()[1];
    buffer[5] = (float) transform.T()[2];
    buffer[6] = (float) axisLength;
    buffer[7] = (float) color[0];
    buffer[8] = (float) color[1];
    buffer[9] = (float) color[2];
    WRITE(outPipe, buffer, 10*sizeof(float));
}


void VisualizationProtocol::addMenu(const String& title, const Array_<pair<String, int> >& items) {
    pthread_mutex_lock(&sceneLock);
    char command = DEFINE_MENU;
    WRITE(outPipe, &command, 1);
    short titleLength = title.size();
    WRITE(outPipe, &titleLength, sizeof(short));
    WRITE(outPipe, title.c_str(), titleLength);
    short numItems = items.size();
    WRITE(outPipe, &numItems, sizeof(short));
    for (int i = 0; i < numItems; i++) {
        int buffer[] = {items[i].second, items[i].first.size()};
        WRITE(outPipe, buffer, 2*sizeof(int));
        WRITE(outPipe, items[i].first.c_str(), items[i].first.size());
    }
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::setCameraTransform(const Transform& transform) const {
    pthread_mutex_lock(&sceneLock);
    char command = SET_CAMERA;
    WRITE(outPipe, &command, 1);
    float buffer[6];
    Vec3 rot = transform.R().convertRotationToBodyFixedXYZ();
    buffer[0] = (float) rot[0];
    buffer[1] = (float) rot[1];
    buffer[2] = (float) rot[2];
    buffer[3] = (float) transform.T()[0];
    buffer[4] = (float) transform.T()[1];
    buffer[5] = (float) transform.T()[2];
    WRITE(outPipe, buffer, 6*sizeof(float));
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::zoomCamera() const {
    pthread_mutex_lock(&sceneLock);
    char command = ZOOM_CAMERA;
    WRITE(outPipe, &command, 1);
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::lookAt(const Vec3& point, const Vec3& upDirection) const {
    pthread_mutex_lock(&sceneLock);
    char command = LOOK_AT;
    WRITE(outPipe, &command, 1);
    float buffer[6];
    buffer[0] = (float) point[0];
    buffer[1] = (float) point[1];
    buffer[2] = (float) point[2];
    buffer[3] = (float) upDirection[0];
    buffer[4] = (float) upDirection[1];
    buffer[5] = (float) upDirection[2];
    WRITE(outPipe, buffer, 6*sizeof(float));
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::setFieldOfView(Real fov) const {
    pthread_mutex_lock(&sceneLock);
    char command = SET_FIELD_OF_VIEW;
    WRITE(outPipe, &command, 1);
    float buffer[1];
    buffer[0] = (float)fov;
    WRITE(outPipe, buffer, sizeof(float));
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::setClippingPlanes(Real near, Real far) const {
    pthread_mutex_lock(&sceneLock);
    char command = SET_CLIP_PLANES;
    WRITE(outPipe, &command, 1);
    float buffer[2];
    buffer[0] = (float)near;
    buffer[1] = (float)far;
    WRITE(outPipe, buffer, 2*sizeof(float));
    pthread_mutex_unlock(&sceneLock);
}

void VisualizationProtocol::setGroundPosition(const CoordinateAxis& axis, Real height) {
    pthread_mutex_lock(&sceneLock);
    char command = SET_GROUND_POSITION;
    WRITE(outPipe, &command, 1);
    float heightBuffer = (float) height;
    WRITE(outPipe, &heightBuffer, sizeof(float));
    short axisBuffer = axis;
    WRITE(outPipe, &axisBuffer, sizeof(short));
    pthread_mutex_unlock(&sceneLock);
}


}