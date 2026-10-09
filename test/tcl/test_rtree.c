/*
** 2010 August 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of dbsql interfaces. This code
** is not included in the dbsql library. 
*/

#include "dbsql.h"
#include "tcl_dbsql.h"

/* Solely for the UNUSED_PARAMETER() macro. */
#include "dbsql_int.h"

#ifdef DBSQL_ENABLE_RTREE
/* 
** Type used to cache parameter information for the "circle" r-tree geometry
** callback.
*/
typedef struct circle_t circle_t;
struct circle_t {
  struct box_t {
    double xmin;
    double xmax;
    double ymin;
    double ymax;
  } boxes[2];
  double centerx;
  double centery;
  double radius;
  double mx_area;
  int score_type;
};

/*
** Destructor function for circle_t objects allocated by circle_geom().
*/
static void circle_del(void *p){
  dbsql_free(p);
}

/*
** Implementation of "circle" r-tree geometry callback.
*/
static int circle_geom(
  dbsql_rtree_geometry_t *p,
  int n_coord, 
  dbsql_rtree_dbl_t *coords,
  int *res
){
  int i;                          /* Iterator variable */
  circle_t *circle;                /* Structure defining circular region */
  double xmin, xmax;              /* X dimensions of box being tested */
  double ymin, ymax;              /* X dimensions of box being tested */

  xmin = coords[0];
  xmax = coords[1];
  ymin = coords[2];
  ymax = coords[3];
  circle = (circle_t *)p->user;
  if( circle==0 ){
    /* If user is still 0, then the parameter values have not been tested
    ** for correctness or stored into a circle_t structure yet. Do this now. */

    /* This geometry callback is for use with a 2-dimensional r-tree table.
    ** Return an error if the table does not have exactly 2 dimensions. */
    if( n_coord!=4 ) return DBSQL_ERROR;

    /* Test that the correct number of parameters (3) have been supplied,
    ** and that the parameters are in range (that the radius of the circle 
    ** radius is greater than zero). */
    if( p->n_param!=3 || p->params[2]<0.0 ) return DBSQL_ERROR;

    /* Allocate a structure to cache parameter data in. Return DBSQL_NOMEM
    ** if the allocation fails. */
    circle = (circle_t *)(p->user = dbsql_malloc(sizeof(circle_t)));
    if( !circle ) return DBSQL_NOMEM;
    p->del_user = circle_del;

    /* Record the center and radius of the circular region. One way that
    ** tested bounding boxes that intersect the circular region are detected
    ** is by testing if each corner of the bounding box lies within radius
    ** units of the center of the circle. */
    circle->centerx = p->params[0];
    circle->centery = p->params[1];
    circle->radius = p->params[2];

    /* Define two bounding box regions. The first, boxes[0], extends to
    ** infinity in the X dimension. It covers the same range of the Y dimension
    ** as the circular region. The second, boxes[1], extends to infinity in
    ** the Y dimension and is constrained to the range of the circle in the
    ** X dimension.
    **
    ** Then imagine each box is split in half along its short axis by a line
    ** that intersects the center of the circular region. A bounding box
    ** being tested can be said to intersect the circular region if it contains
    ** points from each half of either of the two infinite bounding boxes.
    */
    circle->boxes[0].xmin = circle->centerx;
    circle->boxes[0].xmax = circle->centerx;
    circle->boxes[0].ymin = circle->centery + circle->radius;
    circle->boxes[0].ymax = circle->centery - circle->radius;
    circle->boxes[1].xmin = circle->centerx + circle->radius;
    circle->boxes[1].xmax = circle->centerx - circle->radius;
    circle->boxes[1].ymin = circle->centery;
    circle->boxes[1].ymax = circle->centery;
    circle->mx_area = (xmax - xmin)*(ymax - ymin) + 1.0;
  }

  /* Check if any of the 4 corners of the bounding-box being tested lie 
  ** inside the circular region. If they do, then the bounding-box does
  ** intersect the region of interest. Set the output variable to true and
  ** return DBSQL_OK in this case. */
  for(i=0; i<4; i++){
    double x = (i&0x01) ? xmax : xmin;
    double y = (i&0x02) ? ymax : ymin;
    double d2;
    
    d2  = (x-circle->centerx)*(x-circle->centerx);
    d2 += (y-circle->centery)*(y-circle->centery);
    if( d2<(circle->radius*circle->radius) ){
      *res = 1;
      return DBSQL_OK;
    }
  }

  /* Check if the bounding box covers any other part of the circular region.
  ** See comments above for a description of how this test works. If it does
  ** cover part of the circular region, set the output variable to true
  ** and return DBSQL_OK. */
  for(i=0; i<2; i++){
    if( xmin<=circle->boxes[i].xmin 
     && xmax>=circle->boxes[i].xmax 
     && ymin<=circle->boxes[i].ymin 
     && ymax>=circle->boxes[i].ymax 
    ){
      *res = 1;
      return DBSQL_OK;
    }
  }

  /* The specified bounding box does not intersect the circular region. Set
  ** the output variable to zero and return DBSQL_OK. */
  *res = 0;
  return DBSQL_OK;
}

/*
** Implementation of "circle" r-tree geometry callback using the 
** 2nd-generation interface that allows scoring.
**
** Two calling forms:
**
**          Qcircle(X,Y,Radius,type)        -- All values are doubles
**          Qcircle('x:X y:Y r:R e:ETYPE')   -- Single string parameter
*/
static int circle_query_func(dbsql_rtree_query_info_t *p){
  int i;                          /* Iterator variable */
  circle_t *circle;                /* Structure defining circular region */
  double xmin, xmax;              /* X dimensions of box being tested */
  double ymin, ymax;              /* X dimensions of box being tested */
  int n_within = 0;                /* Number of corners inside the circle */

  xmin = p->coords[0];
  xmax = p->coords[1];
  ymin = p->coords[2];
  ymax = p->coords[3];
  circle = (circle_t *)p->user;
  if( circle==0 ){
    /* If user is still 0, then the parameter values have not been tested
    ** for correctness or stored into a circle_t structure yet. Do this now. */

    /* This geometry callback is for use with a 2-dimensional r-tree table.
    ** Return an error if the table does not have exactly 2 dimensions. */
    if( p->n_coord!=4 ) return DBSQL_ERROR;

    /* Test that the correct number of parameters (1 or 4) have been supplied.
    */
    if( p->n_param!=4 && p->n_param!=1 ) return DBSQL_ERROR;

    /* Allocate a structure to cache parameter data in. Return DBSQL_NOMEM
    ** if the allocation fails. */
    circle = (circle_t *)(p->user = dbsql_malloc(sizeof(circle_t)));
    if( !circle ) return DBSQL_NOMEM;
    p->del_user = circle_del;

    /* Record the center and radius of the circular region. One way that
    ** tested bounding boxes that intersect the circular region are detected
    ** is by testing if each corner of the bounding box lies within radius
    ** units of the center of the circle. */
    if( p->n_param==4 ){
      circle->centerx = p->params[0];
      circle->centery = p->params[1];
      circle->radius = p->params[2];
      circle->score_type = (int)p->params[3];
    }else{
      const char *z = (const char*)dbsql_value_text(p->sql_params[0]);
      circle->centerx = 0.0;
      circle->centery = 0.0;
      circle->radius = 0.0;
      circle->score_type = 0;
      while( z && z[0] ){
        if( z[0]=='r' && z[1]==':' ){
          circle->radius = atof(&z[2]);
        }else if( z[0]=='x' && z[1]==':' ){
          circle->centerx = atof(&z[2]);
        }else if( z[0]=='y' && z[1]==':' ){
          circle->centery = atof(&z[2]);
        }else if( z[0]=='e' && z[1]==':' ){
          circle->score_type = (int)atof(&z[2]);
        }else if( z[0]==' ' ){
          z++;
          continue;
        }
        while( z[0]!=0 && z[0]!=' ' ) z++;
        while( z[0]==' ' ) z++;
      }
    }
    if( circle->radius<0.0 ){
      dbsql_free(circle);
      return DBSQL_NOMEM;
    }

    /* Define two bounding box regions. The first, boxes[0], extends to
    ** infinity in the X dimension. It covers the same range of the Y dimension
    ** as the circular region. The second, boxes[1], extends to infinity in
    ** the Y dimension and is constrained to the range of the circle in the
    ** X dimension.
    **
    ** Then imagine each box is split in half along its short axis by a line
    ** that intersects the center of the circular region. A bounding box
    ** being tested can be said to intersect the circular region if it contains
    ** points from each half of either of the two infinite bounding boxes.
    */
    circle->boxes[0].xmin = circle->centerx;
    circle->boxes[0].xmax = circle->centerx;
    circle->boxes[0].ymin = circle->centery + circle->radius;
    circle->boxes[0].ymax = circle->centery - circle->radius;
    circle->boxes[1].xmin = circle->centerx + circle->radius;
    circle->boxes[1].xmax = circle->centerx - circle->radius;
    circle->boxes[1].ymin = circle->centery;
    circle->boxes[1].ymax = circle->centery;
    circle->mx_area = 200.0*200.0;
  }

  /* Check if any of the 4 corners of the bounding-box being tested lie 
  ** inside the circular region. If they do, then the bounding-box does
  ** intersect the region of interest. Set the output variable to true and
  ** return DBSQL_OK in this case. */
  for(i=0; i<4; i++){
    double x = (i&0x01) ? xmax : xmin;
    double y = (i&0x02) ? ymax : ymin;
    double d2;
    
    d2  = (x-circle->centerx)*(x-circle->centerx);
    d2 += (y-circle->centery)*(y-circle->centery);
    if( d2<(circle->radius*circle->radius) ) n_within++;
  }

  /* Check if the bounding box covers any other part of the circular region.
  ** See comments above for a description of how this test works. If it does
  ** cover part of the circular region, set the output variable to true
  ** and return DBSQL_OK. */
  if( n_within==0 ){
    for(i=0; i<2; i++){
      if( xmin<=circle->boxes[i].xmin 
       && xmax>=circle->boxes[i].xmax 
       && ymin<=circle->boxes[i].ymin 
       && ymax>=circle->boxes[i].ymax 
      ){
        n_within = 1;
        break;
      }
    }
  }

  if( circle->score_type==1 ){
    /* Depth first search */
    p->r_score = p->i_level;
  }else if( circle->score_type==2 ){
    /* Breadth first search */
    p->r_score = 100 - p->i_level;
  }else if( circle->score_type==3 ){
    /* Depth-first search, except sort the leaf nodes by area with
    ** the largest area first */
    if( p->i_level==1 ){
      p->r_score = 1.0 - (xmax-xmin)*(ymax-ymin)/circle->mx_area;
      if( p->r_score<0.01 ) p->r_score = 0.01;
    }else{
      p->r_score = 0.0;
    }
  }else if( circle->score_type==4 ){
    /* Depth-first search, except exclude odd rowids */
    p->r_score = p->i_level;
    if( p->i_rowid&1 ) n_within = 0;
  }else{
    /* Breadth-first search, except exclude odd rowids */
    p->r_score = 100 - p->i_level;
    if( p->i_rowid&1 ) n_within = 0;
  }
  if( n_within==0 ){
    p->within = NOT_WITHIN;
  }else if( n_within>=4 ){
    p->within = FULLY_WITHIN;
  }else{
    p->within = PARTLY_WITHIN;
  }
  return DBSQL_OK;
}
/*
** Implementation of "breadthfirstsearch" r-tree geometry callback using the 
** 2nd-generation interface that allows scoring.
**
**     ... WHERE id MATCH breadthfirstsearch($x0,$x1,$y0,$y1) ...
**
** It returns all entries whose bounding boxes overlap with $x0,$x1,$y0,$y1.
*/
static int bfs_query_func(dbsql_rtree_query_info_t *p){
  double x0,x1,y0,y1;        /* Dimensions of box being tested */
  double bx0,bx1,by0,by1;    /* Boundary of the query function */

  if( p->n_param!=4 ) return DBSQL_ERROR;
  x0 = p->coords[0];
  x1 = p->coords[1];
  y0 = p->coords[2];
  y1 = p->coords[3];
  bx0 = p->params[0];
  bx1 = p->params[1];
  by0 = p->params[2];
  by1 = p->params[3];
  p->r_score = 100 - p->i_level;
  if( p->parent_within==FULLY_WITHIN ){
    p->within = FULLY_WITHIN;
  }else if( x0>=bx0 && x1<=bx1 && y0>=by0 && y1<=by1 ){
    p->within = FULLY_WITHIN;
  }else if( x1>=bx0 && x0<=bx1 && y1>=by0 && y0<=by1 ){
    p->within = PARTLY_WITHIN;
  }else{
    p->within = NOT_WITHIN;
  }
  return DBSQL_OK;
}

/* END of implementation of "circle" geometry callback.
**************************************************************************
*************************************************************************/

#include <assert.h>
#include "tcl_dbsql.h"

typedef struct cube_t cube_t;
struct cube_t {
  double x;
  double y;
  double z;
  double width;
  double height;
  double depth;
};

static void cube_context_free(void *p){
  dbsql_free(p);
}

/*
** The context pointer registered along with the 'cube' callback is
** always ((void *)&__g_here). This is just to facilitate testing, it is not
** actually used for anything.
*/
static int __g_here = 42;

/*
** Implementation of a simple r-tree geom callback to test for intersection
** of r-tree rows with a "cube" shape. Cubes are defined by six scalar
** coordinates as follows:
**
**   cube(x, y, z, width, height, depth)
**
** The width, height and depth parameters must all be greater than zero.
*/
static int cube_geom(
  dbsql_rtree_geometry_t *p,
  int n_coord,
  dbsql_rtree_dbl_t *coords,
  int *pi_res
){
  cube_t *cube = (cube_t *)p->user;

  assert( p->context==(void *)&__g_here );

  if( cube==0 ){
    if( p->n_param!=6 || n_coord!=6
     || p->params[3]<=0.0 || p->params[4]<=0.0 || p->params[5]<=0.0
    ){
      return DBSQL_ERROR;
    }
    cube = (cube_t *)dbsql_malloc(sizeof(cube_t));
    if( !cube ){
      return DBSQL_NOMEM;
    }
    cube->x = p->params[0];
    cube->y = p->params[1];
    cube->z = p->params[2];
    cube->width = p->params[3];
    cube->height = p->params[4];
    cube->depth = p->params[5];

    p->user = (void *)cube;
    p->del_user = cube_context_free;
  }

  assert( n_coord==6 );
  *pi_res = 0;
  if( coords[0]<=(cube->x+cube->width)
   && coords[1]>=cube->x
   && coords[2]<=(cube->y+cube->height)
   && coords[3]>=cube->y
   && coords[4]<=(cube->z+cube->depth)
   && coords[5]>=cube->z
  ){
    *pi_res = 1;
  }

  return DBSQL_OK;
}
#endif /* DBSQL_ENABLE_RTREE */

static int DBSQL_TCLAPI register_cube_geom(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifndef DBSQL_ENABLE_RTREE
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(interp);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
#else
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  extern const char *__err_name(int);
  DBSQL *db;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  rc = dbsql_rtree_geometry_callback(db, "cube", cube_geom, (void *)&__g_here);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
#endif
  return TCL_OK;
}

static int DBSQL_TCLAPI register_circle_geom(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#ifndef DBSQL_ENABLE_RTREE
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(interp);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
#else
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  extern const char *__err_name(int);
  DBSQL *db;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  rc = dbsql_rtree_geometry_callback(db, "circle", circle_geom, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_rtree_query_callback(db, "Qcircle",
                                      circle_query_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_rtree_query_callback(db, "breadthfirstsearch",
                                      bfs_query_func, 0, 0);
  }
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
#endif
  return TCL_OK;
}

int Dbsqltestrtree_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "register_cube_geom", register_cube_geom, 0, 0);
  Tcl_CreateObjCommand(interp, "register_circle_geom",register_circle_geom,0,0);
  return TCL_OK;
}
