/* Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "dd_sp.h"

#include "dd_table_share.h"                    // dd_get_mysql_charset
#include "sp.h"                                // SP_DEFAULT_ACCESS_MAPPING
#include "sql_class.h"                         // THD

#include "dd/properties.h"                     // Properties
#include "dd/types/parameter.h"                // dd::Parameter
#include "dd/types/parameter_type_element.h"   // dd::Parameter_type_element


void prepare_sp_chistics_from_dd_routine(const dd::Routine *routine,
                                         st_sp_chistics *sp_chistics)
{
  DBUG_ENTER("prepare_sp_chistics_from_dd_routine");

  sp_chistics->detistic= routine->is_deterministic();

  // SQL Data access.
  switch (routine->sql_data_access())
  {
  case dd::Routine::SDA_NO_SQL:
    sp_chistics->daccess= SP_NO_SQL;
    break;
  case dd::Routine::SDA_CONTAINS_SQL:
    sp_chistics->daccess= SP_CONTAINS_SQL;
    break;
  case dd::Routine::SDA_READS_SQL_DATA:
    sp_chistics->daccess= SP_READS_SQL_DATA;
    break;
  case dd::Routine::SDA_MODIFIES_SQL_DATA:
    sp_chistics->daccess= SP_MODIFIES_SQL_DATA;
    break;
  default:
    sp_chistics->daccess= SP_DEFAULT_ACCESS_MAPPING; /* purecov: deadcode */
  }

  // Security type.
  sp_chistics->suid=
    (routine->security_type() == dd::View::ST_INVOKER) ? SP_IS_NOT_SUID :
                                                         SP_IS_SUID;

  // comment string.
  if (!routine->comment().empty())
  {
    sp_chistics->comment= { routine->comment().c_str(),
                            routine->comment().length() };
  }
  else
    sp_chistics->comment= EMPTY_CSTR;

  DBUG_VOID_RETURN;
}


/**
  Helper method to prepare type in string format from the dd::Parameter's
  object.
  This method is called from the prepare_return_type_string_from_dd_routine()
  and prepare_params_string_from_dd_routine().

  @param[in]    thd      Thread handle.
  @param[in]    param    dd::Parameter's object.
  @param[out]   type_str SQL type string prepared from the dd::Parameter's
                         object.
*/

static void prepare_type_string_from_dd_param(THD *thd,
                                              const dd::Parameter *param,
                                              String *type_str)
{
  DBUG_ENTER("prepare_type_string_from_dd_param");

  // Decimals
  uint numeric_scale= 0;
  if (param->data_type() == dd::enum_column_types::DECIMAL ||
      param->data_type() == dd::enum_column_types::NEWDECIMAL)
    numeric_scale= param->numeric_scale();
  else if (param->data_type() == dd::enum_column_types::FLOAT ||
           param->data_type() == dd::enum_column_types::DOUBLE)
    numeric_scale= param->is_numeric_scale_null() ? NOT_FIXED_DEC :
      param->numeric_scale();

  // ENUM/SET elements.
  TYPELIB *interval= NULL;
  if (param->data_type() == dd::enum_column_types::ENUM ||
      param->data_type() == dd::enum_column_types::SET)
  {
    // Allocate space for interval.
    size_t interval_parts= param->elements_count();

    interval= static_cast<TYPELIB *>(alloc_root(thd->mem_root,
                                                sizeof(TYPELIB)));
    interval->type_names=
      static_cast<const char **>(alloc_root(thd->mem_root,
                                            (sizeof(char *) *
                                             (interval_parts+1))));
    interval->type_names[interval_parts]= 0;

    interval->type_lengths=
      static_cast<uint *>(alloc_root(thd->mem_root,
                                     sizeof(uint) * interval_parts));
    interval->count= interval_parts;
    interval->name= NULL;

    for (const dd::Parameter_type_element *pe : param->elements())
    {
      // Read the enum/set element name
      std::string element_name= pe->name();

      uint pos= pe->index() - 1;
      interval->type_lengths[pos]=
        static_cast<uint>(element_name.length());
      interval->type_names[pos]= strmake_root(thd->mem_root,
                                              element_name.c_str(),
                                              element_name.length());
    }
  }

  // Geometry sub type
  Field::geometry_type geom_type= Field::GEOM_GEOMETRY;
  if (param->data_type() == dd::enum_column_types::GEOMETRY)
  {
    uint32 sub_type;
    dd::Properties *options= const_cast<dd::Properties*>(&param->options());
    options->get_uint32("geom_type", &sub_type);
    geom_type= static_cast<Field::geometry_type>(sub_type);
  }

  // Get type in string format.
  TABLE table;
  TABLE_SHARE share;
  memset(&table, 0, sizeof(table));
  memset(&share, 0, sizeof(share));
  table.in_use= thd;
  table.s= &share;

  std::unique_ptr<Field >
    field(::make_field(table.s, (uchar *)0, param->char_length(),
                       (uchar *)"", 0,
                       dd_get_old_field_type(param->data_type()),
                       dd_get_mysql_charset(param->collation_id()),
                       geom_type, Field::NONE, interval, "", false,
                       param->is_zerofill(), param->is_unsigned(),
                       numeric_scale, 0, 0));
  field->init(&table);
  field->sql_type(*type_str);

  if (field->has_charset())
  {
    type_str->append(STRING_WITH_LEN(" CHARSET "));
    type_str->append(field->charset()->csname);
    if (!(field->charset()->state & MY_CS_PRIMARY))
    {
      type_str->append(STRING_WITH_LEN(" COLLATE "));
      type_str->append(field->charset()->name);
    }
  }

  DBUG_VOID_RETURN;
}


void prepare_return_type_string_from_dd_routine(THD *thd,
                                                const dd::Routine *routine,
                                                std::string *return_type_str)
{
  DBUG_ENTER("prepare_return_type_string_from_dd_routine");

  *return_type_str= "";

  /*
    Return type of Stored function is stored as the first parameter in the data
    dictionary table "Parameters".
    Stored procedures do not have return type so nothing is done for the stored
    procedures.
  */
  if (routine->type() == dd::Routine::RT_FUNCTION)
  {
    const dd::Routine::Parameter_collection &parameters= routine->parameters();

    if (!parameters.is_empty())
    {
      const dd::Parameter *param= *parameters.begin();
      DBUG_ASSERT(param->ordinal_position() == 1);

      String type_str(64);
      type_str.set_charset(system_charset_info);

      prepare_type_string_from_dd_param(thd, param, &type_str);
      *return_type_str= type_str.ptr();
    }
  }

  DBUG_VOID_RETURN;
}


void prepare_params_string_from_dd_routine(THD *thd,
                                           const dd::Routine *routine,
                                           std::string *params_str)
{
  DBUG_ENTER("prepare_params_string_from_dd_routine");

  *params_str= "";

  std::stringstream params_ss;

  for (const dd::Parameter *param : routine->parameters())
  {
    /*
      Return type of stored function is stored as the first parameter. So skip
      it.
    */
    if (routine->type() == dd::Routine::RT_FUNCTION &&
        param->ordinal_position() == 1)
      continue;

    if (params_ss.str().length() > 0)
      params_ss << ", ";

    // PARAMETER MODE
    if (routine->type() == dd::Routine::RT_PROCEDURE)
    {
      switch(param->mode())
      {
      case dd::Parameter::PM_IN:
        params_ss << "IN ";
        break;
      case dd::Parameter::PM_OUT:
        params_ss << "OUT ";
        break;
      case dd::Parameter::PM_INOUT:
        params_ss << "INOUT ";
        break;
      }
    }

    /*
      PARAMETER NAME
      Convert and quote the parameter name if needed.
    */
    String param_str(NAME_LEN + 1);
    sql_mode_t sql_mode= thd->variables.sql_mode;
    thd->variables.sql_mode= routine->sql_mode();
    append_identifier(thd, &param_str, param->name().c_str(),
                      param->name().length());
    thd->variables.sql_mode= sql_mode;
    params_ss << param_str.ptr() << " ";

    // PARAMETER TYPE
    String type_str(64);
    type_str.set_charset(system_charset_info);
    prepare_type_string_from_dd_param(thd, param, &type_str);
    params_ss << type_str.ptr();
  }

  if (params_ss.str().length())
    *params_str= params_ss.str();

  DBUG_VOID_RETURN;
}
