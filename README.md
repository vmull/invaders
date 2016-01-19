Space Invaders Game
===================

Simple space invaders game, did it for university coursework. Should work on all platforms that have GLUT if file_exists/read_jpeg_image is implemented, at the moment it's only implemented for systems that have CoreGraphics (ie. OSX/iPhoneOS) and have the access(...) function (ie. posix systems). The code is stupid and horrible but it does the job. Since the class was about OOP, it's slightly overengineered for the purpose of demonstrating an OOP design. It uses GLUT to handle IO/windows, OpenGL to draw stuff and saves/loads data using a binary stream.

I left images out since they're the property of the university, I believe.