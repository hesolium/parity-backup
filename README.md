# parity-backup
Backup program for disk array. 

In terms of functionality, this program is similar to the popular Snapraid program (https://www.snapraid.it/).
Snapraid protects disk arrays against data loss by periodically creating differential backups of files (in the form of a parity bit stream).
For those interested in the technical specifications and usage details, please refer to the program's documentation.
After using Snapraid for a year, I realized that the program is not well-suited for a multi-user environment (Linux) where data access is restricted.
Specifically, it does not preserve or restore ownership and access permissions for files and directories.
Since my home server runs on Linux and stores data for family members, I decided to create my own (simplified) version of the program that would fully archive and restore the file structure on the protected disks.

Differences between Snapraid (ver. 14.7) and parity-backup:

- parity-backup runs only on Linux, whereas Snapraid is cross-platform.
- parity-backup preserves and restores all file and directory attributes, including ACLs (on file systems that support them)
- disk archiving and restoration operations are fully multi-threaded; in Snapraid, restoring a disk from the archive is single-threaded
- parity-backup protects archived files against unauthorized access: any user can create (or update) the archive, but they can only restore their own files
- the administrator (root) can restore all files (entire disks), and their attributes remain identical to the originals
- parity-backup can recover files to a separate, additional drive (not necessarily one specified in the archive settings)
- parity-backup can only restore files from a single drive in a single session. In contrast, Snapraid's 'fix' operation restores (updates) files across all drives simultaneously
- Both programs are similar regarding file filtering for write and recovery operations. 
  parity-backup additionally supports regular expressions for filtering by filename.

- Snapraid creates a single monolithic archive set, whereas parity-backup stores all organizational information as text files in a subdirectory on the archive drive.
  To improve performance, relationships between individual archive elements are stored in an SQLite database. 
  The SQLite database serves a purely auxiliary function and can be reconstructed from the data stored on the archive drive [using the --rebuild option].

Program usage.
--------------

Before running the program for the first time, you must create a dedicated working directory for additional archive configuration files.
The default path for this directory is /parity-backup. If the user lacks administrator privileges or prefers a different location,
a custom directory can be specified in the program options (using the -c or --config option).
This directory stores small auxiliary files required for the program to function when the archive drive is not available (i.e., not mounted).
Examples include the configuration file, the list of files on the drives being archived, file access permissions, and lists of file relationships. 
Additionally, the program requires a text-based configuration file that defines the basic setup of the drives to be archived. 
When creating the archive, the configuration file must be specified directly in the program's startup argument.
For subsequent runs (updating the archive), it may be omitted.
A description and explanation of the configuration file structure are provided in the included sample configuration file (parityBackupExample.conf).

Building from source.
---------------------

The program consists of several modules (C, C++) and an included module for SQLite database support.

The included 'build-release' script creates the executable version of the program.
You simply need to specify the correct path to the directory containing the 'sqlite3.c' and 'sqlite3.h' files.
