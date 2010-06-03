/*
 * This file Copyright (C) 2009-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSet>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include "hig.h"
#include "relocate.h"
#include "session.h"
#include "torrent.h"
#include "torrent-model.h"

bool RelocateDialog :: myMoveFlag = true;

void
RelocateDialog :: onSetLocation( )
{
    mySession.torrentSetLocation( myIds, myPath, myMoveFlag );
    deleteLater( );
}

void
RelocateDialog :: onFileSelected( const QString& path )
{
    myPath = path;
    myDirButton->setText( myPath );
}

void
RelocateDialog :: onDirButtonClicked( )
{
    if( mySession.isLocal() )
    {
        QFileDialog * d = new QFileDialog( this );
        d->setFileMode( QFileDialog::Directory );
        d->selectFile( myPath );
        d->setOption( QFileDialog::ShowDirsOnly, true );
        connect( d, SIGNAL(fileSelected(const QString&)), this, SLOT(onFileSelected(const QString&)));
        d->show( );
    }
    else
    {
        QInputDialog * d = new QInputDialog( this );
        d->setInputMode( QInputDialog::TextInput );
        d->setWindowTitle( tr( "Set Directory" ) );
        d->setLabelText( tr( "Enter a location:" ) );
        d->setTextValue( myPath );
        connect( d, SIGNAL( textValueSelected( const QString& ) ), this, SLOT( onFileSelected( const QString& ) ) );
        d->show( );
    }
}

void
RelocateDialog :: onMoveToggled( bool b )
{
    myMoveFlag = b;
}

RelocateDialog :: RelocateDialog( Session& session, TorrentModel& model, const QSet<int>& ids, QWidget * parent ):
    QDialog( parent ),
    mySession( session ),
    myModel( model ),
    myIds( ids )
{
    const int iconSize( style( )->pixelMetric( QStyle :: PM_SmallIconSize ) );
    const QFileIconProvider iconProvider;
    const QIcon folderIcon = iconProvider.icon( QFileIconProvider::Folder );
    const QPixmap folderPixmap = folderIcon.pixmap( iconSize );

    QRadioButton * find_rb;
    setWindowTitle( tr( "Set Torrent Location" ) );

    foreach( int id, myIds ) {
        const Torrent * tor = myModel.getTorrentFromId( id );
        if( myPath.isEmpty() )
            myPath = tor->getPath();
        else if( myPath != tor->getPath() )
        {
            if( mySession.isLocal() )
                myPath = QDir::homePath( );
            else
                myPath = QString( "/" );
        }
    }

    HIG * hig = new HIG( );
    hig->addSectionTitle( tr( "Set Location" ) );
    hig->addRow( tr( "New &location:" ), myDirButton = new QPushButton( folderPixmap, myPath ) );
    hig->addWideControl( myMoveRadio = new QRadioButton( tr( "&Move from the current folder" ), this ) );
    hig->addWideControl( find_rb = new QRadioButton( tr( "Local data is &already there" ), this ) );
    hig->finish( );

    if( myMoveFlag )
        myMoveRadio->setChecked( true );
    else
        find_rb->setChecked( true );

    connect( myMoveRadio, SIGNAL(toggled(bool)), this, SLOT(onMoveToggled(bool)));
    connect( myDirButton, SIGNAL(clicked(bool)), this, SLOT(onDirButtonClicked()));

    QLayout * layout = new QVBoxLayout( this );
    layout->addWidget( hig );
    QDialogButtonBox * buttons = new QDialogButtonBox( QDialogButtonBox::Ok|QDialogButtonBox::Cancel );
    connect( buttons, SIGNAL(rejected()), this, SLOT(deleteLater()));
    connect( buttons, SIGNAL(accepted()), this, SLOT(onSetLocation()));
    layout->addWidget( buttons );
    QWidget::setAttribute( Qt::WA_DeleteOnClose, true );
}
