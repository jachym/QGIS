/***************************************************************************
                          qgsauxiliarystorage.cpp  -  description
                            -------------------
    begin                : Aug 28, 2017
    copyright            : (C) 2017 by Paul Blottiere
    email                : paul.blottiere@oslandia.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsauxiliarystorage.h"
#include "qgslogger.h"
#include "qgsslconnect.h"
#include "qgsproject.h"
#include "qgsvectorlayerlabeling.h"
#include "qgsdiagramrenderer.h"
#include "qgsmemoryproviderutils.h"
#include "qgssymbollayer.h"

#include <QFile>

const QString AS_JOINFIELD = "ASPK";
const QString AS_EXTENSION = "qgd";
const QString AS_JOINPREFIX = "auxiliary_storage_";

const QVector<QgsPalLayerSettings::Property> palHiddenProperties
{
  QgsPalLayerSettings::PositionX,
  QgsPalLayerSettings::PositionY,
  QgsPalLayerSettings::Show,
  QgsPalLayerSettings::LabelRotation,
  QgsPalLayerSettings::Family,
  QgsPalLayerSettings::FontStyle,
  QgsPalLayerSettings::Size,
  QgsPalLayerSettings::Bold,
  QgsPalLayerSettings::Italic,
  QgsPalLayerSettings::Underline,
  QgsPalLayerSettings::Color,
  QgsPalLayerSettings::Strikeout,
  QgsPalLayerSettings::BufferSize,
  QgsPalLayerSettings::BufferColor,
  QgsPalLayerSettings::LabelDistance,
  QgsPalLayerSettings::Hali,
  QgsPalLayerSettings::Vali,
  QgsPalLayerSettings::ScaleVisibility,
  QgsPalLayerSettings::MinScale,
  QgsPalLayerSettings::MaxScale,
  QgsPalLayerSettings::AlwaysShow
};

//
// QgsAuxiliaryLayer
//

QgsAuxiliaryLayer::QgsAuxiliaryLayer( const QString &pkField, const QString &filename, const QString &table, QgsVectorLayer *vlayer )
  : QgsVectorLayer( QString( "%1|layername=%2" ).arg( filename, table ), QString( "%1_auxiliarystorage" ).arg( table ), "ogr" )
  , mFileName( filename )
  , mTable( table )
  , mLayer( vlayer )
{
  // init join info
  mJoinInfo.setPrefix( AS_JOINPREFIX );
  mJoinInfo.setJoinLayer( this );
  mJoinInfo.setJoinFieldName( AS_JOINFIELD );
  mJoinInfo.setTargetFieldName( pkField );
  mJoinInfo.setEditable( true );
  mJoinInfo.setUpsertOnEdit( true );
  mJoinInfo.setCascadedDelete( true );
  mJoinInfo.setJoinFieldNamesBlackList( QStringList() << QStringLiteral( "rowid" ) ); // introduced by ogr provider
}

QgsAuxiliaryLayer *QgsAuxiliaryLayer::clone( QgsVectorLayer *target ) const
{
  QgsAuxiliaryStorage::duplicateTable( source(), target->id() );
  return new QgsAuxiliaryLayer( mJoinInfo.targetFieldName(), mFileName, target->id(), target );
}

bool QgsAuxiliaryLayer::clear()
{
  bool rc = deleteFeatures( allFeatureIds() );
  commitChanges();
  startEditing();
  return rc;
}

QgsVectorLayer *QgsAuxiliaryLayer::toSpatialLayer() const
{
  QgsVectorLayer *layer = QgsMemoryProviderUtils::createMemoryLayer( QStringLiteral( "auxiliary_layer" ), fields(), mLayer->wkbType(), mLayer->crs() );

  QString pkField = mJoinInfo.targetFieldName();
  QgsFeature joinFeature;
  QgsFeature targetFeature;
  QgsFeatureIterator it = getFeatures();

  layer->startEditing();
  while ( it.nextFeature( joinFeature ) )
  {
    QString filter = QgsExpression::createFieldEqualityExpression( pkField, joinFeature.attribute( AS_JOINFIELD ) );

    QgsFeatureRequest request;
    request.setFilterExpression( filter );

    mLayer->getFeatures( request ).nextFeature( targetFeature );

    if ( targetFeature.isValid() )
    {
      QgsFeature newFeature( joinFeature );
      newFeature.setGeometry( targetFeature.geometry() );
      layer->addFeature( newFeature );
    }
  }
  layer->commitChanges();

  return layer;
}

QgsVectorLayerJoinInfo QgsAuxiliaryLayer::joinInfo() const
{
  return mJoinInfo;
}

bool QgsAuxiliaryLayer::exists( const QgsPropertyDefinition &definition ) const
{
  return ( indexOfPropertyDefinition( definition ) >= 0 );
}

bool QgsAuxiliaryLayer::addAuxiliaryField( const QgsPropertyDefinition &definition )
{
  if ( ( definition.name().isEmpty() && definition.comment().isEmpty() ) || exists( definition ) )
    return false;

  const QgsField af = createAuxiliaryField( definition );
  const bool rc = addAttribute( af );
  updateFields();
  mLayer->updateFields();

  if ( rc )
  {
    int auxIndex = indexOfPropertyDefinition( definition );
    int index = mLayer->fields().indexOf( nameFromProperty( definition, true ) );

    if ( index >= 0 && auxIndex >= 0 )
    {
      if ( isHiddenProperty( auxIndex ) )
      {
        // update editor widget
        QgsEditorWidgetSetup setup = QgsEditorWidgetSetup( QStringLiteral( "Hidden" ), QVariantMap() );
        setEditorWidgetSetup( auxIndex, setup );

        // column is hidden
        QgsAttributeTableConfig attrCfg = mLayer->attributeTableConfig();
        attrCfg.update( mLayer->fields() );
        QVector<QgsAttributeTableConfig::ColumnConfig> columns = attrCfg.columns();
        QVector<QgsAttributeTableConfig::ColumnConfig>::iterator it;

        for ( it = columns.begin(); it != columns.end(); ++it )
        {
          if ( it->name.compare( mLayer->fields().field( index ).name() ) == 0 )
            it->hidden = true;
        }

        attrCfg.setColumns( columns );
        mLayer->setAttributeTableConfig( attrCfg );
      }
      else if ( definition.standardTemplate() == QgsPropertyDefinition::ColorNoAlpha
                || definition.standardTemplate() == QgsPropertyDefinition::ColorWithAlpha )
      {
        QgsEditorWidgetSetup setup = QgsEditorWidgetSetup( QStringLiteral( "Color" ), QVariantMap() );
        setEditorWidgetSetup( auxIndex, setup );
      }

      mLayer->setEditorWidgetSetup( index, editorWidgetSetup( auxIndex ) );
    }
  }

  return rc;
}

QgsFields QgsAuxiliaryLayer::auxiliaryFields() const
{
  QgsFields afields;

  for ( int i = 2; i < fields().count(); i++ ) // ignore rowid and PK field
    afields.append( createAuxiliaryField( fields().field( i ) ) );

  return afields;
}

bool QgsAuxiliaryLayer::deleteAttribute( int attr )
{
  QgsVectorLayer::deleteAttribute( attr );
  bool rc = commitChanges();
  startEditing();
  return rc;
}

bool QgsAuxiliaryLayer::save()
{
  bool rc = false;

  if ( isEditable() )
  {
    rc = commitChanges();
  }

  startEditing();

  return rc;
}

int QgsAuxiliaryLayer::createProperty( QgsPalLayerSettings::Property property, QgsVectorLayer *layer )
{
  int index = -1;

  if ( layer && layer->labeling() && layer->auxiliaryLayer() )
  {
    // property definition are identical whatever the provider id
    const QgsPropertyDefinition def = layer->labeling()->settings().propertyDefinitions()[property];
    const QString fieldName = nameFromProperty( def, true );

    layer->auxiliaryLayer()->addAuxiliaryField( def );

    if ( layer->auxiliaryLayer()->indexOfPropertyDefinition( def ) >= 0 )
    {
      const QgsProperty prop = QgsProperty::fromField( fieldName );

      for ( const QString &providerId : layer->labeling()->subProviders() )
      {
        QgsPalLayerSettings *settings = new QgsPalLayerSettings( layer->labeling()->settings( providerId ) );

        QgsPropertyCollection c = settings->dataDefinedProperties();
        c.setProperty( property, prop );
        settings->setDataDefinedProperties( c );

        layer->labeling()->setSettings( settings, providerId );
      }

      emit layer->styleChanged();
    }

    index = layer->fields().lookupField( fieldName );
  }

  return index;
}

int QgsAuxiliaryLayer::createProperty( QgsDiagramLayerSettings::Property property, QgsVectorLayer *layer )
{
  int index = -1;

  if ( layer && layer->diagramLayerSettings() && layer->auxiliaryLayer() )
  {
    const QgsPropertyDefinition def = layer->diagramLayerSettings()->propertyDefinitions()[property];

    if ( layer->auxiliaryLayer()->addAuxiliaryField( def ) )
    {
      const QString fieldName = nameFromProperty( def, true );
      const QgsProperty prop = QgsProperty::fromField( fieldName );

      QgsDiagramLayerSettings settings( *layer->diagramLayerSettings() );

      QgsPropertyCollection c = settings.dataDefinedProperties();
      c.setProperty( property, prop );
      settings.setDataDefinedProperties( c );

      layer->setDiagramLayerSettings( settings );
      emit layer->styleChanged();

      index = layer->fields().lookupField( fieldName );
    }
  }

  return index;
}

bool QgsAuxiliaryLayer::isHiddenProperty( int index ) const
{
  bool hidden = false;
  QgsPropertyDefinition def = propertyDefinitionFromIndex( index );

  if ( def.origin().compare( "labeling" ) == 0 )
  {
    for ( const QgsPalLayerSettings::Property &p : palHiddenProperties )
    {
      const QString propName = QgsPalLayerSettings::propertyDefinitions()[ p ].name();
      if ( propName.compare( def.name() ) == 0 )
      {
        hidden = true;
        break;
      }
    }
  }

  return hidden;
}

int QgsAuxiliaryLayer::propertyFromIndex( int index ) const
{
  int p = -1;
  QgsPropertyDefinition aDef = propertyDefinitionFromIndex( index );

  if ( aDef.origin().compare( QStringLiteral( "labeling" ) ) == 0 )
  {
    const QgsPropertiesDefinition defs = QgsPalLayerSettings::propertyDefinitions();
    QgsPropertiesDefinition::const_iterator it = defs.constBegin();
    for ( ; it != defs.constEnd(); ++it )
    {
      if ( it->name().compare( aDef.name(), Qt::CaseInsensitive ) == 0 )
      {
        p = it.key();
        break;
      }
    }
  }
  else if ( aDef.origin().compare( QStringLiteral( "symbol" ) ) == 0 )
  {
    const QgsPropertiesDefinition defs = QgsSymbolLayer::propertyDefinitions();
    QgsPropertiesDefinition::const_iterator it = defs.constBegin();
    for ( ; it != defs.constEnd(); ++it )
    {
      if ( it->name().compare( aDef.name(), Qt::CaseInsensitive ) == 0 )
      {
        p = it.key();
        break;
      }
    }
  }
  else if ( aDef.origin().compare( QStringLiteral( "diagram" ) ) == 0 )
  {
    const QgsPropertiesDefinition defs = QgsDiagramLayerSettings::propertyDefinitions();
    QgsPropertiesDefinition::const_iterator it = defs.constBegin();
    for ( ; it != defs.constEnd(); ++it )
    {
      if ( it->name().compare( aDef.name(), Qt::CaseInsensitive ) == 0 )
      {
        p = it.key();
        break;
      }
    }
  }

  return p;
}

QgsPropertyDefinition QgsAuxiliaryLayer::propertyDefinitionFromIndex( int index ) const
{
  return propertyDefinitionFromField( fields().field( index ) );
}

int QgsAuxiliaryLayer::indexOfPropertyDefinition( const QgsPropertyDefinition &def ) const
{
  return fields().indexOf( nameFromProperty( def ) );
}

QString QgsAuxiliaryLayer::nameFromProperty( const QgsPropertyDefinition &def, bool joined )
{
  QString fieldName = def.origin();

  if ( !def.name().isEmpty() )
    fieldName =  QString( "%1_%2" ).arg( fieldName, def.name().toLower() );

  if ( !def.comment().isEmpty() )
    fieldName = QString( "%1_%2" ).arg( fieldName ).arg( def.comment() );

  if ( joined )
    fieldName = QString( "%1%2" ).arg( AS_JOINPREFIX, fieldName );

  return fieldName;
}

QgsField QgsAuxiliaryLayer::createAuxiliaryField( const QgsPropertyDefinition &def )
{
  QgsField afield;

  if ( !def.name().isEmpty() || !def.comment().isEmpty() )
  {
    QVariant::Type type;
    QString typeName;
    int len( 0 ), precision( 0 );
    switch ( def.dataType() )
    {
      case QgsPropertyDefinition::DataTypeString:
        type = QVariant::String;
        len = 50;
        typeName = "String";
        break;
      case QgsPropertyDefinition::DataTypeNumeric:
        type = QVariant::Double;
        len = 0;
        precision = 0;
        typeName = "Real";
        break;
      case QgsPropertyDefinition::DataTypeBoolean:
        type = QVariant::Int; // sqlite does not have a bool type
        typeName = "Integer";
        break;
    }

    afield.setType( type );
    afield.setName( nameFromProperty( def ) );
    afield.setTypeName( typeName );
    afield.setLength( len );
    afield.setPrecision( precision );
  }

  return afield;
}

QgsPropertyDefinition QgsAuxiliaryLayer::propertyDefinitionFromField( const QgsField &f )
{
  QgsPropertyDefinition def;
  const QStringList parts = f.name().split( '_' );

  if ( parts.size() <= 1 )
    return def;

  const QString origin = parts[0];
  const QString propertyName = parts[1];

  if ( origin.compare( "labeling", Qt::CaseInsensitive ) == 0 )
  {
    const QgsPropertiesDefinition props = QgsPalLayerSettings::propertyDefinitions();
    for ( const QgsPropertyDefinition &p : props.values() )
    {
      if ( p.name().compare( propertyName, Qt::CaseInsensitive ) == 0 )
      {
        def = p;
        if ( parts.size() == 3 )
          def.setComment( parts[2] );
        break;
      }
    }
  }
  else if ( origin.compare( "symbol", Qt::CaseInsensitive ) == 0 )
  {
    const QgsPropertiesDefinition props = QgsSymbolLayer::propertyDefinitions();
    for ( const QgsPropertyDefinition &p : props.values() )
    {
      if ( p.name().compare( propertyName, Qt::CaseInsensitive ) == 0 )
      {
        def = p;
        if ( parts.size() == 3 )
          def.setComment( parts[2] );
        break;
      }
    }
  }
  else if ( origin.compare( "diagram", Qt::CaseInsensitive ) == 0 )
  {
    const QgsPropertiesDefinition props = QgsDiagramLayerSettings::propertyDefinitions();
    for ( const QgsPropertyDefinition &p : props.values() )
    {
      if ( p.name().compare( propertyName, Qt::CaseInsensitive ) == 0 )
      {
        def = p;
        if ( parts.size() == 3 )
          def.setComment( parts[2] );
        break;
      }
    }
  }
  else
  {
    def.setOrigin( origin );
    def.setName( propertyName );

    if ( parts.size() == 3 )
      def.setComment( parts[2] );
  }

  return def;
}

QgsField QgsAuxiliaryLayer::createAuxiliaryField( const QgsField &field )
{
  QgsPropertyDefinition def = propertyDefinitionFromField( field );
  QgsField afield;

  if ( !def.name().isEmpty() || !def.comment().isEmpty() )
  {
    afield = createAuxiliaryField( def );
    afield.setTypeName( field.typeName() );
  }

  return afield;
}

//
// QgsAuxiliaryStorage
//

QgsAuxiliaryStorage::QgsAuxiliaryStorage( const QgsProject &project, bool copy )
  : mCopy( copy )
{
  initTmpFileName();

  if ( !project.fileInfo().fileName().isEmpty() )
  {
    const QFileInfo info = project.fileInfo();
    const QString path = info.path() + QDir::separator() + info.baseName();
    const QString asFileName = path + "." + QgsAuxiliaryStorage::extension();
    mFileName = asFileName;
  }

  sqlite3 *handler = open( mFileName );
  close( handler );
}

QgsAuxiliaryStorage::QgsAuxiliaryStorage( const QString &filename, bool copy )
  : mFileName( filename )
  , mCopy( copy )
{
  initTmpFileName();

  sqlite3 *handler = open( filename );
  close( handler );
}

QgsAuxiliaryStorage::~QgsAuxiliaryStorage()
{
  QFile::remove( mTmpFileName );
}

bool QgsAuxiliaryStorage::isValid() const
{
  return mValid;
}

QString QgsAuxiliaryStorage::fileName() const
{
  return mFileName;
}

bool QgsAuxiliaryStorage::save() const
{
  if ( mFileName.isEmpty() )
  {
    // only a saveAs is available on a new database
    return false;
  }
  else if ( mCopy )
  {
    if ( QFile::exists( mFileName ) )
      QFile::remove( mFileName );

    return QFile::copy( mTmpFileName, mFileName );
  }
  else
  {
    // if the file is not empty the copy mode is not activated, then we're
    // directly working on the database since the beginning (no savepoints
    // /rollback for now)
    return true;
  }
}

QgsAuxiliaryLayer *QgsAuxiliaryStorage::createAuxiliaryLayer( const QgsField &field, QgsVectorLayer *layer ) const
{
  QgsAuxiliaryLayer *alayer = nullptr;

  if ( mValid && layer )
  {
    const QString table( layer->id() );
    sqlite3 *handler = openDB( currentFileName() );

    if ( !tableExists( table, handler ) )
    {
      if ( !createTable( field.typeName(), table, handler ) )
      {
        close( handler );
        return alayer;
      }
    }

    alayer = new QgsAuxiliaryLayer( field.name(), currentFileName(), table, layer );
    alayer->startEditing();
    close( handler );
  }

  return alayer;
}

bool QgsAuxiliaryStorage::deleteTable( const QgsDataSourceUri &ogrUri )
{
  bool rc = false;
  QgsDataSourceUri uri = parseOgrUri( ogrUri );

  if ( !uri.database().isEmpty() && !uri.table().isEmpty() )
  {
    sqlite3 *handler = openDB( uri.database() );

    if ( handler )
    {
      QString sql = QString( "DROP TABLE %1" ).arg( uri.table() );
      rc = exec( sql, handler );

      sql = QString( "VACUUM" );
      rc = exec( sql, handler );

      close( handler );
    }
  }

  return rc;
}

bool QgsAuxiliaryStorage::duplicateTable( const QgsDataSourceUri &ogrUri, const QString &newTable )
{
  QgsDataSourceUri uri = parseOgrUri( ogrUri );
  bool rc = false;

  if ( !uri.table().isEmpty() && !uri.database().isEmpty() )
  {
    sqlite3 *handler = openDB( uri.database() );

    if ( handler )
    {
      QString sql = QString( "CREATE TABLE %1 AS SELECT * FROM %2" ).arg( newTable, uri.table() );
      rc = exec( sql, handler );

      close( handler );
    }
  }

  return rc;
}

bool QgsAuxiliaryStorage::saveAs( const QString &filename ) const
{
  if ( QFile::exists( filename ) )
    QFile::remove( filename );

  return  QFile::copy( currentFileName(), filename );
}

bool QgsAuxiliaryStorage::saveAs( const QgsProject &project ) const
{
  return saveAs( filenameForProject( project ) );
}

QString QgsAuxiliaryStorage::extension()
{
  return AS_EXTENSION;
}

bool QgsAuxiliaryStorage::exec( const QString &sql, sqlite3 *handler )
{
  bool rc = false;

  if ( handler )
  {
    const int err = sqlite3_exec( handler, sql.toStdString().c_str(), nullptr, nullptr, nullptr );

    if ( err == SQLITE_OK )
      rc = true;
    else
      debugMsg( sql, handler );
  }

  return rc;
}

void QgsAuxiliaryStorage::debugMsg( const QString &sql, sqlite3 *handler )
{
  const QString err = QString::fromUtf8( sqlite3_errmsg( handler ) );
  const QString msg = QObject::tr( "Unable to execute" );
  const QString errMsg = QObject::tr( "%1 '%2': %3" ).arg( msg ).arg( sql ).arg( err );
  QgsDebugMsg( errMsg );
}

sqlite3 *QgsAuxiliaryStorage::openDB( const QString &filename )
{
  sqlite3 *handler = nullptr;

  bool rc = QgsSLConnect::sqlite3_open_v2( filename.toUtf8().constData(), &handler, SQLITE_OPEN_READWRITE, nullptr );
  if ( rc )
  {
    debugMsg( "sqlite3_open_v2", handler );
    return nullptr;
  }

  return handler;
}

bool QgsAuxiliaryStorage::createTable( const QString &type, const QString &table, sqlite3 *handler )
{
  const QString sql = QString( "CREATE TABLE IF NOT EXISTS '%1' ( '%2' %3  )" ).arg( table ).arg( AS_JOINFIELD ).arg( type );

  if ( !exec( sql, handler ) )
    return false;

  return true;
}

sqlite3 *QgsAuxiliaryStorage::createDB( const QString &filename )
{
  sqlite3 *handler = nullptr;
  int rc;

  // open/create database
  rc = QgsSLConnect::sqlite3_open_v2( filename.toUtf8().constData(), &handler, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr );
  if ( rc )
  {
    debugMsg( "sqlite3_open_v2", handler );
    return handler;
  }

  // activating Foreign Key constraints
  if ( !exec( "PRAGMA foreign_keys = 1", handler ) )
    return handler;

  return handler;
}

bool QgsAuxiliaryStorage::tableExists( const QString &table, sqlite3 *handler )
{
  const QString sql = QString( "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%1'" ).arg( table );
  int rows = 0;
  int columns = 0;
  char **results = nullptr;
  const int rc = sqlite3_get_table( handler, sql.toStdString().c_str(), &results, &rows, &columns, nullptr );
  if ( rc != SQLITE_OK )
  {
    debugMsg( sql, handler );
    return false;
  }

  sqlite3_free_table( results );
  if ( rows >= 1 )
    return true;

  return false;
}

sqlite3 *QgsAuxiliaryStorage::open( const QString &filename )
{
  sqlite3 *handler = nullptr;

  if ( filename.isEmpty() )
  {
    if ( ( handler = createDB( currentFileName() ) ) )
      mValid = true;
  }
  else if ( QFile::exists( filename ) )
  {
    if ( mCopy )
      QFile::copy( filename, mTmpFileName );

    if ( ( handler = openDB( currentFileName() ) ) )
      mValid = true;
  }
  else
  {
    if ( ( handler = createDB( currentFileName() ) ) )
      mValid = true;
  }

  return handler;
}

sqlite3 *QgsAuxiliaryStorage::open( const QgsProject &project )
{
  return open( filenameForProject( project ) );
}

void QgsAuxiliaryStorage::close( sqlite3 *handler )
{
  if ( handler )
  {
    QgsSLConnect::sqlite3_close_v2( handler );
    handler = nullptr;
  }
}

QString QgsAuxiliaryStorage::filenameForProject( const QgsProject &project )
{
  const QFileInfo info = project.fileInfo();
  const QString path = info.path() + QDir::separator() + info.baseName();
  return path + "." + QgsAuxiliaryStorage::extension();
}

void QgsAuxiliaryStorage::initTmpFileName()
{
  QTemporaryFile tmpFile;
  tmpFile.open();
  tmpFile.close();
  mTmpFileName = tmpFile.fileName();
}

QString QgsAuxiliaryStorage::currentFileName() const
{
  if ( mCopy || mFileName.isEmpty() )
    return mTmpFileName;
  else
    return mFileName;
}

QgsDataSourceUri QgsAuxiliaryStorage::parseOgrUri( const QgsDataSourceUri &uri )
{
  QgsDataSourceUri newUri;

  // parsing for ogr style uri :
  // " filePath|layername='tableName' table="" sql="
  QStringList uriParts = uri.uri().split( '|' );
  if ( uriParts.count() < 2 )
    return newUri;

  const QString databasePath = uriParts[0].replace( ' ', "" );

  const QString table = uriParts[1];
  QStringList tableParts = table.split( ' ' );

  if ( tableParts.count() < 1 )
    return newUri;

  const QString tableName = tableParts[0].replace( "layername=", "" );

  newUri.setDataSource( QString(), tableName, QString() );
  newUri.setDatabase( databasePath );

  return newUri;
}
